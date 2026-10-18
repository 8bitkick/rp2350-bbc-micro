/*
 * beebjit's os_* platform layer for a small embedded machine: one thread, no
 * MMU, no window system. Used by the RP2350 (Presto) port and by the AArch32
 * Linux test harness, which differ only in beebjit_host.h.
 *
 * How the desktop concepts map:
 * - Threads: os_thread_create() runs the thread function to completion, on the
 *   caller's stack. bbc_run_async() therefore runs the emulator and returns
 *   when it exits.
 * - Channels (bbc thread <-> UI thread messages): in-memory queues. A reader
 *   can register a handler that is called as soon as a message is written, so
 *   the "UI" side runs synchronously inside the emulator's vsync callback.
 * - Memory mappings: there is no MMU, so a "mapping" of a memory handle is
 *   just a pointer into it, and protection changes do nothing. bbc.c is built
 *   with BEEBJIT_NO_MMU so it doesn't depend on aliased mappings or guard
 *   pages.
 * - Large allocations can go to a separate region (PSRAM on the Presto); see
 *   beebjit_embedded_malloc().
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "os_alloc.h"
#include "os_channel.h"
#include "os_fault.h"
#include "os_lock.h"
#include "os_poller.h"
#include "os_terminal.h"
#include "os_thread.h"
#include "os_time.h"
#include "util.h"

#include "beebjit_host.h"
#include "os_embedded.h"
#include "../../common/presto_boot.h"

#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* Upstream's null implementations, as used by its headless build. */
#include "os_fault_platform_null.c"
#include "os_sound_null.c"

/* ------------------------------------------------------------------------ */
/* Allocation. */

/* Allocations at least this big go to the big region, if there is one. */
#ifndef BEEBJIT_BIG_ALLOC_THRESHOLD
#define BEEBJIT_BIG_ALLOC_THRESHOLD (128 * 1024)
#endif

/* A first-fit allocator for the big region. Few, large, long-lived blocks, so
 * nothing clever. Each block has a header; free blocks are merged with free
 * neighbours.
 */
struct big_block {
  uint32_t size;      /* Including this header; a multiple of 16. */
  uint32_t is_free;
  uint32_t pad[2];
};

static uint8_t* s_big_base;
static uint8_t* s_big_end;
static size_t s_big_used;
static size_t s_big_peak;
static size_t s_small_live;

static void
big_init(void) {
  static int s_inited;
  uint8_t* p_base;
  size_t size;
  struct big_block* p_block;

  if (s_inited) {
    return;
  }
  s_inited = 1;
  size = beebjit_host_big_region(&p_base);
  if (size < 4096) {
    return;
  }
  /* 16-byte alignment. */
  size -= (16 - ((uintptr_t) p_base & 15)) & 15;
  p_base += (16 - ((uintptr_t) p_base & 15)) & 15;
  size &= ~(size_t) 15;
  s_big_base = p_base;
  s_big_end = (p_base + size);
  p_block = (struct big_block*) p_base;
  p_block->size = size;
  p_block->is_free = 1;
}

static int
big_owns(void* p) {
  return ((uint8_t*) p >= s_big_base) && ((uint8_t*) p < s_big_end);
}

static void*
big_alloc(size_t size) {
  uint8_t* p;
  uint32_t need = ((size + sizeof(struct big_block) + 15) & ~15u);

  for (p = s_big_base; p < s_big_end; p += ((struct big_block*) p)->size) {
    struct big_block* p_block = (struct big_block*) p;
    if (!p_block->is_free || (p_block->size < need)) {
      continue;
    }
    if ((p_block->size - need) >= 64) {
      struct big_block* p_rest = (struct big_block*) (p + need);
      p_rest->size = (p_block->size - need);
      p_rest->is_free = 1;
      p_block->size = need;
    }
    p_block->is_free = 0;
    s_big_used += p_block->size;
    if (s_big_used > s_big_peak) {
      s_big_peak = s_big_used;
    }
    return (p + sizeof(struct big_block));
  }
  return NULL;
}

static void
big_free(void* p) {
  uint8_t* q;
  struct big_block* p_block = (struct big_block*) ((uint8_t*) p -
                                                   sizeof(struct big_block));
  assert(!p_block->is_free);
  p_block->is_free = 1;
  s_big_used -= p_block->size;
  /* Merge runs of free blocks. */
  for (q = s_big_base; q < s_big_end; q += ((struct big_block*) q)->size) {
    struct big_block* p_q = (struct big_block*) q;
    while (p_q->is_free) {
      struct big_block* p_next = (struct big_block*) (q + p_q->size);
      if (((uint8_t*) p_next >= s_big_end) || !p_next->is_free) {
        break;
      }
      p_q->size += p_next->size;
    }
  }
}

static size_t
big_size(void* p) {
  struct big_block* p_block = (struct big_block*) ((uint8_t*) p -
                                                   sizeof(struct big_block));
  return (p_block->size - sizeof(struct big_block));
}

/* The harness's allocation log names the caller (util.c's hook). */
static void* s_alloc_caller;

void*
beebjit_embedded_malloc(size_t size) {
  void* p_ret = NULL;
  big_init();
  if ((size >= BEEBJIT_BIG_ALLOC_THRESHOLD) && (s_big_base != NULL)) {
    p_ret = big_alloc(size);
  }
  if (p_ret == NULL) {
    /* (an out-of-memory panic then reports this size and the SRAM total) */
    presto_boot_note(size, s_small_live);
    p_ret = malloc(size);
    if (p_ret != NULL) {
      s_small_live += size;
    }
  }
  if (beebjit_embedded_log_allocs) {
    struct mallinfo m = mallinfo();
    (void) printf("alloc %u -> %s %p (heap end %p, %u free in heap) from %p\n",
                  (unsigned) size,
                  big_owns(p_ret) ? "big" : "ram",
                  p_ret,
                  sbrk(0),
                  (unsigned) m.fordblks,
                  s_alloc_caller);
    s_alloc_caller = NULL;
  }
  return p_ret;
}

void*
beebjit_embedded_malloc_big(size_t size) {
  void* p_ret = NULL;
  big_init();
  if (s_big_base != NULL) {
    p_ret = big_alloc(size);
  }
  if (beebjit_embedded_log_allocs) {
    (void) printf("alloc big %u -> %s %p (big region %u of %u used)\n",
                  (unsigned) size,
                  (p_ret != NULL) ? "big" : "ram",
                  p_ret,
                  (unsigned) s_big_used,
                  (unsigned) (s_big_end - s_big_base));
  }
  if (p_ret == NULL) {
    p_ret = util_malloc(size);
  }
  return p_ret;
}

void
beebjit_embedded_free(void* p) {
  if (p == NULL) {
    return;
  }
  if (big_owns(p)) {
    big_free(p);
  } else {
    free(p);
  }
}

void*
beebjit_embedded_realloc(void* p, size_t size) {
  void* p_new;
  size_t old_size;
  if ((p == NULL) || !big_owns(p)) {
    if ((p == NULL) && (size >= BEEBJIT_BIG_ALLOC_THRESHOLD)) {
      return beebjit_embedded_malloc(size);
    }
    return realloc(p, size);
  }
  old_size = big_size(p);
  if (size <= old_size) {
    return p;
  }
  p_new = beebjit_embedded_malloc(size);
  if (p_new != NULL) {
    (void) memcpy(p_new, p, old_size);
    big_free(p);
  }
  return p_new;
}

int beebjit_embedded_log_allocs;
void
beebjit_embedded_alloc_caller(void* p_caller) {
  s_alloc_caller = p_caller;
}

void
beebjit_embedded_get_alloc_stats(size_t* p_big_used,
                                 size_t* p_big_peak,
                                 size_t* p_small_total) {
  *p_big_used = s_big_used;
  *p_big_peak = s_big_peak;
  *p_small_total = s_small_live;
}

/* Mappings. With no MMU, every mapping of a memory handle is the handle's own
 * memory. bbc.c (with BEEBJIT_NO_MMU) only asks for one.
 */
struct os_alloc_mapping {
  void* p_addr;
  int is_owned;
};

void*
os_alloc_get_aligned(size_t alignment, size_t size) {
  void* p_ret;
  size_t padded = (size + alignment);
  uint8_t* p_raw = util_malloc(padded + sizeof(void*));
  uintptr_t addr = (uintptr_t) (p_raw + sizeof(void*));
  addr = ((addr + alignment - 1) & ~(uintptr_t) (alignment - 1));
  p_ret = (void*) addr;
  ((void**) p_ret)[-1] = p_raw;
  return p_ret;
}

void
os_alloc_free_aligned(void* p) {
  util_free(((void**) p)[-1]);
}

intptr_t
os_alloc_get_memory_handle(size_t size) {
  /* 16 spare bytes before the memory: the thumb2 JIT keeps the 6502's D and I
   * flags there, so its code reaches them from the memory base register (see
   * asm/thumb2/thumb2_defs.h).
   */
  uint8_t* p = util_mallocz(size + 16);
  return (intptr_t) (p + 16);
}

void
os_alloc_free_memory_handle(intptr_t handle) {
  util_free((uint8_t*) handle - 16);
}

void*
os_alloc_get_mapping_addr(struct os_alloc_mapping* p_mapping) {
  return p_mapping->p_addr;
}

struct os_alloc_mapping*
os_alloc_get_mapping_from_handle(intptr_t handle,
                                 void* p_addr,
                                 size_t offset,
                                 size_t size) {
  struct os_alloc_mapping* p_mapping =
      util_mallocz(sizeof(struct os_alloc_mapping));
  (void) p_addr;
  (void) size;
  p_mapping->p_addr = ((uint8_t*) handle + offset);
  p_mapping->is_owned = 0;
  return p_mapping;
}

struct os_alloc_mapping*
os_alloc_get_mapping(void* p_addr, size_t size) {
  struct os_alloc_mapping* p_mapping =
      util_mallocz(sizeof(struct os_alloc_mapping));
  void* p_fixed = NULL;
  /* A JIT mapping must be at the address asked for: the host may have it. */
  if (p_addr != NULL) {
    p_fixed = beebjit_host_fixed_region(p_addr, size);
  }
  if (p_fixed != NULL) {
    (void) memset(p_fixed, '\0', size);
    p_mapping->p_addr = p_fixed;
    p_mapping->is_owned = 0;
    return p_mapping;
  }
  p_mapping->p_addr = util_mallocz(size);
  p_mapping->is_owned = 1;
  return p_mapping;
}

void
os_alloc_free_mapping(struct os_alloc_mapping* p_mapping) {
  if (p_mapping == NULL) {
    return;
  }
  if (p_mapping->is_owned) {
    util_free(p_mapping->p_addr);
  }
  util_free(p_mapping);
}

void
os_alloc_make_mapping_read_only(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
}

void
os_alloc_make_mapping_read_write(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
}

void
os_alloc_make_mapping_read_write_exec(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
}

void
os_alloc_make_mapping_read_exec(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
}

void
os_alloc_make_mapping_none(void* p_addr, size_t size) {
  (void) p_addr;
  (void) size;
}

/* ------------------------------------------------------------------------ */
/* Channels. Handles 0-1 are the BBC -> client queue (read, write), 2-3 the
 * client -> BBC queue, matching the order os_channel_get_handles() returns
 * them in.
 */

enum {
  k_channel_queue_bytes = 1024,
};

struct channel_queue {
  uint8_t buf[k_channel_queue_bytes];
  uint32_t head;
  uint32_t tail;
  void (*p_handler)(void* p);
  void* p_handler_object;
};

static struct channel_queue s_queues[2];

void
os_channel_get_handles(intptr_t* p_read1,
                       intptr_t* p_write1,
                       intptr_t* p_read2,
                       intptr_t* p_write2) {
  (void) memset(s_queues, '\0', sizeof(s_queues));
  /* read1 is the client's read end, write1 the BBC's write end. */
  *p_read1 = 0;
  *p_write1 = 1;
  *p_read2 = 2;
  *p_write2 = 3;
}

void
os_channel_free_handles(intptr_t read1,
                        intptr_t write1,
                        intptr_t read2,
                        intptr_t write2) {
  (void) read1;
  (void) write1;
  (void) read2;
  (void) write2;
}

void
os_channel_embedded_set_handler(intptr_t read_handle,
                                void (*p_handler)(void* p),
                                void* p_handler_object) {
  struct channel_queue* p_queue = &s_queues[read_handle >> 1];
  p_queue->p_handler = p_handler;
  p_queue->p_handler_object = p_handler_object;
}

void
os_channel_read(intptr_t handle, void* p_message, uint32_t length) {
  uint32_t i;
  struct channel_queue* p_queue = &s_queues[handle >> 1];
  uint8_t* p_out = (uint8_t*) p_message;
  for (i = 0; i < length; ++i) {
    if (p_queue->head == p_queue->tail) {
      util_bail("os_channel_read would block forever (single threaded)");
    }
    p_out[i] = p_queue->buf[p_queue->tail % k_channel_queue_bytes];
    p_queue->tail++;
  }
}

void
os_channel_write(intptr_t handle, const void* p_message, uint32_t length) {
  uint32_t i;
  struct channel_queue* p_queue = &s_queues[handle >> 1];
  const uint8_t* p_in = (const uint8_t*) p_message;
  for (i = 0; i < length; ++i) {
    if ((p_queue->head - p_queue->tail) == k_channel_queue_bytes) {
      util_bail("os_channel_write queue full");
    }
    p_queue->buf[p_queue->head % k_channel_queue_bytes] = p_in[i];
    p_queue->head++;
  }
  if (p_queue->p_handler != NULL) {
    p_queue->p_handler(p_queue->p_handler_object);
  }
}

/* ------------------------------------------------------------------------ */
/* Threads: run to completion. */

struct os_thread_struct {
  void* p_ret;
};

struct os_thread_struct*
os_thread_create(void* p_func, void* p_arg) {
  static struct os_thread_struct s_thread;
  void* (*p_thread_func)(void*) = (void* (*)(void*)) p_func;
  s_thread.p_ret = p_thread_func(p_arg);
  return &s_thread;
}

intptr_t
os_thread_destroy(struct os_thread_struct* p_thread_struct) {
  return (intptr_t) p_thread_struct->p_ret;
}

/* Locks: nothing to lock against. */
struct os_lock_struct {
  int dummy;
};

struct os_lock_struct*
os_lock_create(void) {
  return util_mallocz(sizeof(struct os_lock_struct));
}

void
os_lock_destroy(struct os_lock_struct* p_lock) {
  util_free(p_lock);
}

void
os_lock_lock(struct os_lock_struct* p_lock) {
  (void) p_lock;
}

void
os_lock_unlock(struct os_lock_struct* p_lock) {
  (void) p_lock;
}

/* ------------------------------------------------------------------------ */
/* Time. */

void
os_time_setup_hi_res(void) {
}

uint64_t
os_time_get_us(void) {
  return beebjit_host_time_us();
}

struct os_time_sleeper*
os_time_create_sleeper(void) {
  return NULL;
}

void
os_time_free_sleeper(struct os_time_sleeper* p_sleeper) {
  (void) p_sleeper;
}

void
os_time_sleeper_sleep_us(struct os_time_sleeper* p_sleeper, uint64_t us) {
  (void) p_sleeper;
  beebjit_host_sleep_us(us);
}

/* ------------------------------------------------------------------------ */
/* Terminal: the host's byte stream. */

intptr_t
os_terminal_get_stdin_handle(void) {
  return 0;
}

intptr_t
os_terminal_get_stdout_handle(void) {
  return 1;
}

void
os_terminal_setup(intptr_t handle) {
  (void) handle;
}

static int s_term_has_byte;
static uint8_t s_term_byte;

int
os_terminal_has_readable_bytes(intptr_t handle) {
  (void) handle;
  if (!s_term_has_byte) {
    s_term_has_byte = beebjit_host_term_read(&s_term_byte);
  }
  return s_term_has_byte;
}

int
os_terminal_handle_read_byte(intptr_t handle, uint8_t* p_byte) {
  if (!os_terminal_has_readable_bytes(handle)) {
    return 0;
  }
  *p_byte = s_term_byte;
  s_term_has_byte = 0;
  return 1;
}

int
os_terminal_handle_write_byte(intptr_t handle, uint8_t byte) {
  (void) handle;
  beebjit_host_term_write(byte);
  return 1;
}

void
os_terminal_set_ctrl_c_callback(void (*p_interrupt_callback)(void)) {
  (void) p_interrupt_callback;
}

/* ------------------------------------------------------------------------ */
/* Faults: no MMU faults to field; the JIT, which relies on them, isn't used. */

void
os_fault_register_handler(
    void (*p_fault_callback)(uintptr_t* p_host_rip,
                             uintptr_t host_fault_addr,
                             int is_illegal,
                             int is_exec,
                             int is_write,
                             uintptr_t host_rdi)) {
  (void) p_fault_callback;
}

void
os_fault_bail(void) {
  abort();
}

void
os_debug_trap(void) {
  abort();
}

/* Poller: only used by upstream's main.c. */
struct os_poller_struct {
  int dummy;
};

struct os_poller_struct*
os_poller_create(void) {
  return util_mallocz(sizeof(struct os_poller_struct));
}

void
os_poller_destroy(struct os_poller_struct* p_poller) {
  util_free(p_poller);
}

void
os_poller_add_handle(struct os_poller_struct* p_poller, intptr_t handle) {
  (void) p_poller;
  (void) handle;
}

void
os_poller_poll(struct os_poller_struct* p_poller) {
  (void) p_poller;
}

int
os_poller_handle_triggered(struct os_poller_struct* p_poller, uint32_t index) {
  (void) p_poller;
  (void) index;
  return 0;
}
