#include "jit.h"

#include "bbc_options.h"
#include "cpu_driver.h"
#include "debug.h"
#include "defs_6502.h"
#include "interp.h"
#include "inturbo.h"
#include "memory_access.h"
#include "os_alloc.h"
#include "os_fault.h"
#include "jit_compiler.h"
#include "jit_metadata.h"
#include "jit_opcode.h"
#include "log.h"
#include "state_6502.h"
#include "timing.h"
#include "util.h"

#include "asm/asm_common.h"
#include "asm/asm_defs_host.h"
#include "asm/asm_inturbo.h"
#include "asm/asm_inturbo_defs.h"
#include "asm/asm_jit.h"
#include "asm/asm_jit_defs.h"
#include "asm/asm_opcodes.h"

#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void* g_p_jit_base = (void*) NULL;

struct jit_struct {
  /* Fields referenced by the JIT code. */
  struct cpu_driver driver;

  /* C callbacks called by JIT code. */
  void* p_compile_callback;

  /* C pointers used by JIT code. */
  struct inturbo_struct* p_inturbo;

  /* 6502 address -> JIT code pointers.
   * These are stored as 32-bit even when the mapping is hosted in 64-bit
   * space. It's hoped to be more cache efficient.
   */
  uint32_t jit_ptrs[k_6502_addr_space_size];

  /* Context pointer for JIT-defined custom callbacks. */
  void* p_jit_callback_context;

  /* Fields not referenced by JIT code. */
  struct asm_jit_struct* p_asm;
  struct jit_metadata* p_metadata;
  struct os_alloc_mapping* p_mapping_jit;
  struct os_alloc_mapping* p_mapping_no_code_ptr;
  uint8_t* p_jit_base;
  struct jit_compiler* p_compiler;
  struct util_buffer* p_temp_buf;
  struct interp_struct* p_interp;
  uint8_t* p_opcode_types;
  uint8_t* p_opcode_modes;
  uint8_t* p_opcode_mem;
  uint8_t* p_opcode_cycles;
  uint64_t last_housekeeping_cycles;

  int log_compile;
  int log_fault;

  uint64_t counter_num_compiles;
  uint64_t counter_num_interps;
  uint64_t counter_num_faults;
  int do_fault_log;
  int is_encoded_callbacks;
  /* For the interpreter's per-instruction callback: the metadata's code
   * blocks, and whether the interpreter has a memory written callback (as
   * set through jit_set_memory_written_callback()).
   */
  const int32_t* p_code_blocks;
  int has_memory_written_callback;
  /* BEEBJIT_JIT_EARLY_RETURN: how the current interpreter run started. */
  int interp_return_mode;
  uint32_t interp_entry_fires;
};

/* BEEBJIT_JIT_IO_STAY: JIT code bounces to the interpreter for each hardware
 * register access that isn't an encoded callback. A bounce costs far more
 * than interpreting an instruction, so stay in the interpreter through
 * bursts of register accesses and polling loops. Either way the 6502 runs
 * exactly the same; this only picks the cheaper one.
 * Off since the thumb2 backend has encoded callbacks: the look-ahead costs
 * every interpreted instruction more than the bounces it saves (Bar Beeb,
 * which bounces for each video timer expiry: 90% -> 99% of real speed).
 */
#ifndef BEEBJIT_JIT_IO_STAY
#define BEEBJIT_JIT_IO_STAY 0
#endif

/* BEEBJIT_JIT_EARLY_RETURN: upstream's interpreter runs on to the next JIT
 * block start before going back to JIT code, which in a long block (a raster
 * loop) can be dozens of instructions per bounce. Instead, go back as soon
 * as the reason for the bounce is dealt with: bit 0, for an expiring
 * countdown, once a timer has fired; bit 1, otherwise after the instruction
 * that bounced. A PC in the middle of a block then becomes a block start (the
 * block is split), as for any jump into a block.
 * Off: on the Presto it halves the interpreter's time in Bar Beeb, but the
 * splits spread Elite's hot code over more of the JIT's 64-byte slots, and
 * the XIP cache misses cost more (Elite turbo 8.3x -> 5.7x).
 */
#ifndef BEEBJIT_JIT_EARLY_RETURN
#define BEEBJIT_JIT_EARLY_RETURN 0
#endif
enum {
  k_jit_interp_return_block = 0,
  k_jit_interp_return_after_one = 1,
  k_jit_interp_return_after_fire = 2,
};

#if BEEBJIT_JIT_IO_STAY
/* Whether JIT code makes this access itself, as an encoded callback (see
 * jit_compiler.c: known address reads that don't write, and stores).
 */
static int
jit_is_encoded_access(struct jit_struct* p_jit,
                      uint8_t opmode,
                      uint8_t opmem,
                      uint16_t addr) {
  struct memory_access* p_memory_access =
      p_jit->driver.p_extra->p_memory_access;
  struct asm_uop uops[k_max_uops_per_opcode];
  int ends_block;
  uint32_t extra_cycles;
  void* p_obj = p_memory_access->p_callback_obj;

  if (!p_jit->is_encoded_callbacks || (opmode != k_abs)) {
    return 0;
  }
  if (opmem == k_opmem_read_flag) {
    return (p_memory_access->memory_get_read_jit_encoding(
                p_obj, uops, &ends_block, &extra_cycles,
                k_max_uops_per_opcode, addr, 1) > 0);
  }
  if (opmem == k_opmem_write_flag) {
    return (p_memory_access->memory_get_write_jit_encoding(
                p_obj, uops, &ends_block, &extra_cycles,
                k_max_uops_per_opcode, addr, 1) > 0);
  }
  return 0;
}

/* Whether the instruction at pc accesses a hardware register (by its base
 * address, for indexed modes) without an encoded callback.
 */
static int
jit_is_register_access(struct jit_struct* p_jit, uint16_t pc) {
  struct memory_access* p_memory_access =
      p_jit->driver.p_extra->p_memory_access;
  uint8_t* p_mem_read = p_memory_access->p_mem_read;
  uint8_t opcode = p_mem_read[pc];
  uint8_t opmode = p_jit->p_opcode_modes[opcode];
  uint8_t opmem = p_jit->p_opcode_mem[opcode];
  uint16_t addr;
  void* p_obj = p_memory_access->p_callback_obj;

  if ((opmode != k_abs) && (opmode != k_abx) && (opmode != k_aby)) {
    return 0;
  }
  addr = (p_mem_read[(uint16_t) (pc + 1)] |
          (p_mem_read[(uint16_t) (pc + 2)] << 8));
  if ((opmem & k_opmem_read_flag) &&
      p_memory_access->memory_read_needs_callback(p_obj, addr)) {
    return !jit_is_encoded_access(p_jit, opmode, opmem, addr);
  }
  if ((opmem & k_opmem_write_flag) &&
      p_memory_access->memory_write_needs_callback(p_obj, addr)) {
    return !jit_is_encoded_access(p_jit, opmode, opmem, addr);
  }
  return 0;
}
#endif

static int
jit_interp_instruction_callback(void* p,
                                uint16_t next_pc,
                                uint8_t done_opcode,
                                uint16_t done_addr,
                                int next_is_irq,
                                int irq_pending) {
  int32_t next_block;
  int32_t next_block_prev;
  uint8_t opmem;

  struct jit_metadata* p_metadata;
  struct jit_struct* p_jit = (struct jit_struct*) p;

  p_metadata = p_jit->p_metadata;
  opmem = p_jit->p_opcode_mem[done_opcode];

  /* Any memory writes executed by the interpreter need to invalidate
   * compiled JIT code if they're self-modifying writes.
   */
  if ((opmem & k_opmem_write_flag) && (p_jit->p_code_blocks[done_addr] != -1)) {
    void* p_jit_ptr = jit_metadata_get_host_jit_ptr(p_metadata, done_addr);
    if (!jit_metadata_is_jit_ptr_no_code(p_metadata, p_jit_ptr) &&
        !jit_metadata_is_jit_ptr_dynamic(p_metadata, p_jit_ptr)) {
      asm_jit_start_code_updates(p_jit->p_asm, p_jit_ptr, 4);
      asm_jit_invalidate_code_at(p_jit_ptr);
      asm_jit_finish_code_updates(p_jit->p_asm);
    }
  }

  if (next_is_irq || irq_pending) {
    /* Keep interpreting to handle the IRQ. */
    return 0;
  }

#if BEEBJIT_JIT_EARLY_RETURN
  if (p_jit->interp_return_mode != k_jit_interp_return_block) {
    if ((p_jit->interp_return_mode == k_jit_interp_return_after_one) ||
        (timing_get_num_fires(p_jit->driver.p_extra->p_timing) !=
         p_jit->interp_entry_fires)) {
      if (!interp_has_memory_written_callback(p_jit->p_interp)) {
        p_jit->interp_return_mode = k_jit_interp_return_block;
        return 1;
      }
    }
  }
#endif

  /* We stay in interp indefinitely if we're syncing the 6502 writes to video
   * 6845 reads. This is denoted by the presence of a memory written handler.
   */
  if (p_jit->has_memory_written_callback) {
    assert(interp_has_memory_written_callback(p_jit->p_interp));
    return 0;
  }
  assert(!interp_has_memory_written_callback(p_jit->p_interp));

#if BEEBJIT_JIT_IO_STAY
  {
    /* Another register access within the next few straight-line
     * instructions: it would bounce straight back.
     */
    uint8_t* p_mem_read = p_jit->driver.p_extra->p_memory_access->p_mem_read;
    uint16_t pc = next_pc;
    uint32_t i;
    for (i = 0; i < 3; ++i) {
      uint8_t opcode = p_mem_read[pc];
      if (jit_is_register_access(p_jit, pc)) {
        return 0;
      }
      if (g_opbranch[p_jit->p_opcode_types[opcode]] != k_bra_n) {
        break;
      }
      pc += g_opmodelens[p_jit->p_opcode_modes[opcode]];
    }
  }
  if ((p_jit->p_opcode_modes[done_opcode] != k_rel) &&
      jit_is_register_access(p_jit, done_addr)) {
    /* A branch back to (just before) the access: a polling loop. */
    uint8_t* p_mem_read = p_jit->driver.p_extra->p_memory_access->p_mem_read;
    uint8_t next_opcode = p_mem_read[next_pc];
    if (p_jit->p_opcode_modes[next_opcode] == k_rel) {
      uint16_t target = (uint16_t) (next_pc + 2 +
                                    (int8_t) p_mem_read[(uint16_t) (next_pc +
                                                                    1)]);
      if ((target <= done_addr) && ((uint16_t) (done_addr - target) <= 8)) {
        return 0;
      }
    }
  }
#endif

  next_block = p_jit->p_code_blocks[next_pc];
  if (next_block == -1) {
    /* Always consider an address with no JIT code to be a new block
     * boundary. Without this, an RTI to an uncompiled region will stay stuck
     * in the interpreter.
     */
    return 1;
  }

  next_block_prev = p_jit->p_code_blocks[(uint16_t) (next_pc - 1)];
  if (next_block != next_block_prev) {
    /* If the instruction we're about to execute is at the start of a JIT
     * block, bounce back into JIT at this clean boundary.
     */
    return 1;
  }

  /* Keep interpreting. */
  return 0;
}

struct jit_enter_interp_ret {
  int64_t countdown;
  int64_t exited;
};

static void
jit_enter_interp(struct jit_struct* p_jit,
                 struct jit_enter_interp_ret* p_ret,
                 int64_t countdown,
                 uint64_t host_flags) {
  uint32_t cpu_driver_flags;

  struct cpu_driver* p_jit_cpu_driver = &p_jit->driver;
  struct jit_compiler* p_compiler = p_jit->p_compiler;
  struct interp_struct* p_interp = p_jit->p_interp;
  struct state_6502* p_state_6502 = p_jit_cpu_driver->abi.p_state_6502;

  p_jit->counter_num_interps++;

#if BEEBJIT_JIT_EARLY_RETURN
  /* A negative countdown: the countdown check at a block start bounced (a
   * timer is due inside the block); anything else bounces one instruction.
   */
  if (countdown < 0) {
    p_jit->interp_return_mode = ((BEEBJIT_JIT_EARLY_RETURN & 1) ?
                                 k_jit_interp_return_after_fire :
                                 k_jit_interp_return_block);
    p_jit->interp_entry_fires =
        timing_get_num_fires(p_jit_cpu_driver->p_extra->p_timing);
  } else {
    p_jit->interp_return_mode = ((BEEBJIT_JIT_EARLY_RETURN & 2) ?
                                 k_jit_interp_return_after_one :
                                 k_jit_interp_return_block);
  }
#endif

  /* Take care of any deferred fault logging. */
  if (p_jit->do_fault_log) {
    p_jit->do_fault_log = 0;
    log_do_log(k_log_jit, k_log_info, "JIT handled fault (log every 10k)");
  }

  /* Bouncing out of the JIT is quite jarring. We need to fixup up any state
   * that was temporarily stale due to optimizations.
   */
  countdown = jit_compiler_fixup_state(p_compiler,
                                       p_state_6502,
                                       countdown,
                                       host_flags,
                                       0);

  countdown = interp_enter_with_countdown(p_interp, countdown);

  cpu_driver_flags = p_jit_cpu_driver->p_funcs->get_flags(p_jit_cpu_driver);
  p_ret->countdown = countdown;
  p_ret->exited = !!(cpu_driver_flags & k_cpu_flag_exited);
}

static void
jit_destroy(struct cpu_driver* p_cpu_driver) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_inturbo_cpu_driver =
      (struct cpu_driver*) p_jit->p_inturbo;
  struct cpu_driver* p_interp_cpu_driver = (struct cpu_driver*) p_jit->p_interp;

  jit_metadata_destroy(p_jit->p_metadata);
  asm_jit_destroy(p_jit->p_asm);

  os_alloc_free_mapping(p_jit->p_mapping_no_code_ptr);

  if (p_inturbo_cpu_driver != NULL) {
    p_inturbo_cpu_driver->p_funcs->destroy(p_inturbo_cpu_driver);
  }
  p_interp_cpu_driver->p_funcs->destroy(p_interp_cpu_driver);

  util_buffer_destroy(p_jit->p_temp_buf);

  jit_compiler_destroy(p_jit->p_compiler);

  os_alloc_free_mapping(p_jit->p_mapping_jit);

  os_alloc_free_aligned(p_cpu_driver);
}

static int
jit_enter(struct cpu_driver* p_cpu_driver) {
  int exited;
  int64_t countdown;

  struct timing_struct* p_timing = p_cpu_driver->p_extra->p_timing;
  struct state_6502* p_state_6502 = p_cpu_driver->abi.p_state_6502;
  uint16_t addr_6502 = state_6502_get_pc(p_state_6502);
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  void* p_start_addr = jit_metadata_get_host_block_address(p_metadata,
                                                           addr_6502);
  void* p_mem_base = (void*) K_BBC_MEM_READ_IND_ADDR;

  countdown = timing_get_countdown(p_timing);

  /* The memory must be aligned to at least 0x100 so that our register access
   * tricks work.
   */
  assert(((uintptr_t) p_mem_base & 0xff) == 0);

  exited = asm_jit_enter(p_jit, p_start_addr, countdown, p_mem_base);
  assert(exited == 1);

  return exited;
}

static void
jit_set_reset_callback(struct cpu_driver* p_cpu_driver,
                       void (*do_reset_callback)(void* p, uint32_t flags),
                       void* p_do_reset_callback_object) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;

  p_interp_driver->p_funcs->set_reset_callback(p_interp_driver,
                                               do_reset_callback,
                                               p_do_reset_callback_object);
}

static void
jit_set_memory_written_callback(struct cpu_driver* p_cpu_driver,
                                void (*memory_written_callback)(void* p),
                                void* p_memory_written_callback_object) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;
  p_jit->has_memory_written_callback = (memory_written_callback != NULL);
  p_interp_driver->p_funcs->set_memory_written_callback(
      p_interp_driver,
      memory_written_callback,
      p_memory_written_callback_object);
}

static void
jit_apply_flags(struct cpu_driver* p_cpu_driver,
                uint32_t flags_set,
                uint32_t flags_clear) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;

  p_interp_driver->p_funcs->apply_flags(p_interp_driver,
                                        flags_set,
                                        flags_clear);
}

static uint32_t
jit_get_flags(struct cpu_driver* p_cpu_driver) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;

  return p_interp_driver->p_funcs->get_flags(p_interp_driver);
}


static uint32_t
jit_get_exit_value(struct cpu_driver* p_cpu_driver) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;

  return p_interp_driver->p_funcs->get_exit_value(p_interp_driver);
}

static void
jit_set_exit_value(struct cpu_driver* p_cpu_driver, uint32_t exit_value) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct cpu_driver* p_interp_driver = (struct cpu_driver*) p_jit->p_interp;

  p_interp_driver->p_funcs->set_exit_value(p_interp_driver, exit_value);
}

static void
jit_memory_range_invalidate(struct cpu_driver* p_cpu_driver,
                            uint16_t addr_6502,
                            uint32_t len) {
  uint32_t i;
  void* p_block_ptr;

  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  uint32_t addr_end_6502 = (addr_6502 + len);

  assert(len <= k_6502_addr_space_size);
  assert(addr_end_6502 <= k_6502_addr_space_size);

  if (p_jit->log_compile) {
    log_do_log(k_log_jit,
               k_log_info,
               "invalidate range $%.4X-$%.4X",
               addr_6502,
               (addr_end_6502 - 1));
  }

  assert(addr_end_6502 >= addr_6502);

  p_block_ptr = jit_metadata_get_host_block_address(p_metadata, addr_6502);
  asm_jit_start_code_updates(p_jit->p_asm,
                             p_block_ptr,
                             (len * K_JIT_BYTES_PER_BYTE));

  for (i = addr_6502; i < addr_end_6502; ++i) {
    int32_t code_block = jit_metadata_get_code_block(p_metadata, i);
    /* We assume we're not executing in the middle of a JIT block. Therefore,
     * we can invalidate the entire range simply by making sure the very
     * start of every code block is invalidated. This is possible now that
     * the invalidation pointer is inclusive of any block countdown prefix.
     */
    if (code_block == (int32_t) i) {
      void* p_jit_ptr = jit_metadata_get_host_jit_ptr(p_metadata, i);
      asm_jit_invalidate_code_at(p_jit_ptr);
    }
    if (code_block != -1) {
      jit_metadata_set_code_block(p_metadata, i, -1);
    }
    jit_metadata_make_jit_ptr_no_code(p_metadata, i);
  }

  asm_jit_finish_code_updates(p_jit->p_asm);

  jit_compiler_memory_range_invalidate(p_jit->p_compiler, addr_6502, len);
}

static char*
jit_get_address_info(struct cpu_driver* p_cpu_driver, uint16_t addr) {
  static char block_addr_buf[5];

  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  int32_t block_addr_6502 = jit_metadata_get_code_block(p_metadata, addr);

  (void) snprintf(block_addr_buf,
                  sizeof(block_addr_buf),
                  "%.4X",
                  (uint16_t) block_addr_6502);

  return block_addr_buf;
}

static void
jit_get_custom_counters(struct cpu_driver* p_cpu_driver,
                        uint64_t* p_c1,
                        uint64_t* p_c2) {
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;

  *p_c1 = p_jit->counter_num_compiles;
  *p_c2 = p_jit->counter_num_interps;
}

static void
jit_check_code_block(struct jit_struct* p_jit, uint16_t block_addr_6502) {
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  int32_t code_block = jit_metadata_get_code_block(p_metadata, block_addr_6502);
  int has_invalidations = 0;
  uint16_t addr_6502;
  void* p_block_ptr;

  assert(code_block == block_addr_6502);

  addr_6502 = block_addr_6502;
  while (code_block == block_addr_6502) {
    void* p_jit_ptr = jit_metadata_get_host_jit_ptr(p_metadata, addr_6502);
    assert(!jit_metadata_is_jit_ptr_no_code(p_metadata, p_jit_ptr));
    if (jit_metadata_is_jit_ptr_dynamic(p_metadata, p_jit_ptr)) {
      /* No action. */
    } else if (asm_jit_is_invalidated_code_at(p_jit_ptr)) {
      has_invalidations = 1;
      break;
    }
    addr_6502++;
    code_block = jit_metadata_get_code_block(p_metadata, addr_6502);
  }

  if (!has_invalidations) {
    return;
  }

  if (p_jit->log_compile) {
    log_do_log(k_log_jit,
               k_log_info,
               "stale code block at $%.4X",
               block_addr_6502);
  }

  /* Clear out jit pointers and the code block metadata. */
  addr_6502 = block_addr_6502;
  code_block = jit_metadata_get_code_block(p_metadata, addr_6502);
  while (code_block == block_addr_6502) {
    void* p_jit_ptr = jit_metadata_get_host_jit_ptr(p_metadata, addr_6502);
    assert(!jit_metadata_is_jit_ptr_no_code(p_metadata, p_jit_ptr));
    if (jit_metadata_is_jit_ptr_dynamic(p_metadata, p_jit_ptr)) {
      /* No action. */
    } else if (asm_jit_is_invalidated_code_at(p_jit_ptr)) {
      /* This stopgap measure attempts to prevent write invalidation faults on
       * ARM64. If we get here, and there's an invalidated code address, it's
       * probably because that code isn't ever being executed. If it was
       * being executed, it would typically get compiled to a dynamic operand
       * or dynamic opcode.
       * We tag the address so that a dynamic opcode will be compiled, and this
       * will prevent write invalidation faults.
       * Examples: Thurst, Galaforce.
       */
      jit_compiler_tag_address_as_dynamic(p_jit->p_compiler, addr_6502);
    }
    jit_metadata_make_jit_ptr_no_code(p_metadata, addr_6502);
    jit_metadata_set_code_block(p_metadata, addr_6502, -1);
    /* TODO: clear compiler metadata? */
    addr_6502++;
    code_block = jit_metadata_get_code_block(p_metadata, addr_6502);
  }

  /* Disable the code block itself. */
  p_block_ptr = jit_metadata_get_host_block_address(p_metadata,
                                                    block_addr_6502);
  asm_jit_start_code_updates(p_jit->p_asm, p_block_ptr, 4);
  asm_jit_invalidate_code_at(p_block_ptr);
  asm_jit_finish_code_updates(p_jit->p_asm);
}

static void
jit_cleanup_stale_code(struct jit_struct* p_jit) {
  uint32_t i;
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  int32_t curr_code_block = -1;

  log_do_log(k_log_jit, k_log_info, "starting stale code sweep");

  for (i = 0; i < k_6502_addr_space_size; ++i) {
    int32_t next_code_block = jit_metadata_get_code_block(p_metadata, i);
    if (next_code_block == curr_code_block) {
      continue;
    }
    if (next_code_block != -1) {
      jit_check_code_block(p_jit, i);
    }
    curr_code_block = next_code_block;
  }
}

static void
jit_housekeeping_tick(struct cpu_driver* p_cpu_driver) {
  static const uint64_t k_cycles_threshold = (2000000 * 60 * 5);
  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct state_6502* p_state_6502 = p_cpu_driver->abi.p_state_6502;
  uint64_t cycles = state_6502_get_cycles(p_state_6502);

  /* Some time after 6502 reset (currently 5 minutes of virtual time), sweep
   * the JIT code space and clear out any code blocks containing invalidations.
   * Such code blocks are likely no longer active, but contribute an overhead,
   * especially on ARM64, where writing a code invalidation pointer faults.
   */
  if (p_jit->last_housekeeping_cycles < k_cycles_threshold) {
    if (cycles >= k_cycles_threshold) {
      jit_cleanup_stale_code(p_jit);
    }
  }

  p_jit->last_housekeeping_cycles = cycles;
}

static int64_t
jit_compile(struct jit_struct* p_jit,
            uint8_t* p_host_pc,
            int64_t countdown,
            uint64_t host_flags) {
  uint32_t bytes_6502_compiled;
  uint16_t addr_6502;
  uint16_t addr_6502_end;
  uint16_t addr_6502_last;
  void* p_jit_block;
  void* p_jit_block_end;

  struct state_6502* p_state_6502 = p_jit->driver.abi.p_state_6502;
  struct jit_compiler* p_compiler = p_jit->p_compiler;
  struct jit_metadata* p_metadata = p_jit->p_metadata;
  int32_t code_block_6502;
  int is_invalidation = 0;
  int has_6502_code = 0;
  int is_block_continuation = 0;
  int do_redo_prepare = 0;

  p_jit->counter_num_compiles++;

  addr_6502 = jit_metadata_get_6502_pc_from_host_pc(p_metadata, p_host_pc);
  code_block_6502 = jit_metadata_get_code_block(p_metadata, addr_6502);
  if (asm_jit_is_invalidated_code_at(p_host_pc)) {
    if (((uintptr_t) p_host_pc & (K_JIT_BYTES_PER_BYTE - 1)) != 0) {
      /* Middle of a code block. Must be an invalidation. */
      is_invalidation = 1;
    } else if (addr_6502 == code_block_6502) {
      /* Very beginning of code block. Must be an invalidation. */
      is_invalidation = 1;
    }
  }

  /* Bouncing out of the JIT is quite jarring. We need to fixup up any state
   * that was temporarily stale due to optimizations.
   */
  p_state_6502->abi_state.reg_pc = addr_6502;
  if (is_invalidation) {
    countdown = jit_compiler_fixup_state(p_compiler,
                                         p_state_6502,
                                         countdown,
                                         host_flags,
                                         1);
  }

  if (p_jit->log_compile) {
    has_6502_code = jit_metadata_is_pc_in_code_block(p_metadata, addr_6502);
    is_block_continuation = jit_compiler_is_block_continuation(p_compiler,
                                                               addr_6502);
  }

  /* Get the compile bounds. */
  bytes_6502_compiled = jit_compiler_prepare_compile_block(p_compiler,
                                                           is_invalidation,
                                                           addr_6502);
  addr_6502_end = (addr_6502 + bytes_6502_compiled);

  if ((addr_6502 < 0xFF) &&
      !jit_compiler_is_compiling_for_code_in_zero_page(p_compiler)) {
    log_do_log(k_log_jit,
               k_log_unusual,
               "compiling zero page code @$%.2X",
               addr_6502);

    /* Invalidate all existing compiled code because if it writes to the zero
     * page, it isn't doing self-modified code correctly.
     */
    jit_memory_range_invalidate(&p_jit->driver,
                                0,
                                (k_6502_addr_space_size - 1));

    jit_compiler_set_compiling_for_code_in_zero_page(p_compiler, 1);
    do_redo_prepare = 1;
  }
  if ((addr_6502 <= 0x1FF) && (addr_6502_end >= 0x100)) {
    log_do_log(k_log_jit,
               k_log_unimplemented,
               "compiling stack page code @$%.4X; self-modify not handled",
               addr_6502);
  }

  if (do_redo_prepare) {
    /* Some compilation options changed, so-generate the compiler structures. */
    bytes_6502_compiled = jit_compiler_prepare_compile_block(p_compiler,
                                                             is_invalidation,
                                                             addr_6502);
    addr_6502_end = (addr_6502 + bytes_6502_compiled);
  }

  /* Prepare for and write out the new binary code. */
  p_jit_block = jit_metadata_get_host_block_address(p_metadata, addr_6502);
  p_jit_block_end =
      ((uint8_t*) p_jit_block + (bytes_6502_compiled * K_JIT_BYTES_PER_BYTE));
  assert(p_jit_block_end <= (void*) K_JIT_ADDR_END);
  asm_jit_start_code_updates(
      p_jit->p_asm,
      p_jit_block,
      ((uint8_t*) p_jit_block_end - (uint8_t*) p_jit_block));
  jit_compiler_execute_compile_block(p_compiler);
  asm_jit_finish_code_updates(p_jit->p_asm);

  /* Handle any overlap with existing code blocks. */
  if ((code_block_6502 != -1) && (code_block_6502 != addr_6502)) {
    /* We're splitting a code block before, so invalidate it. */
    void* p_jit_ptr = jit_metadata_get_host_block_address(p_metadata,
                                                          code_block_6502);
    asm_jit_start_code_updates(p_jit->p_asm, p_jit_ptr, 4);
    asm_jit_invalidate_code_at(p_jit_ptr);
    asm_jit_finish_code_updates(p_jit->p_asm);

    jit_metadata_clear_block(p_metadata, code_block_6502);
  }
  addr_6502_end = (addr_6502 + bytes_6502_compiled);
  addr_6502_last = (addr_6502_end - 1);
  code_block_6502 = jit_metadata_get_code_block(p_metadata, addr_6502_end);
  if ((code_block_6502 != -1) && (code_block_6502 <= addr_6502_last)) {
    /* We're splitting a code block after, so invalidate it. */
    jit_metadata_clear_block(p_metadata, addr_6502_end);
  }

  if (p_jit->log_compile) {
    const char* p_text;
    if (is_invalidation) {
      p_text = "inval";
    } else if (is_block_continuation) {
      p_text = "cont";
    } else if (has_6502_code) {
      p_text = "split";
    } else {
      p_text = "new";
    }
    log_do_log(k_log_jit,
               k_log_info,
               "compile @$%.4X-$%.4X [host 0x%"PRIx64"], %s at ticks %"PRIu64,
               addr_6502,
               addr_6502_last,
               (uint64_t) p_host_pc,
               p_text,
               timing_get_total_timer_ticks(p_jit->driver.p_extra->p_timing));
  }

  return countdown;
}

static void
jit_safe_hex_convert(char* p_buf, void* p_ptr) {
  size_t i;
  size_t val = (size_t) p_ptr;
  for (i = 0; i < 8; ++i) {
    char c1 = (val & 0x0f);
    char c2 = ((val & 0xf0) >> 4);

    if (c1 < 10) {
      c1 = '0' + c1;
    } else {
      c1 = 'a' + (c1 - 10);
    }
    if (c2 < 10) {
      c2 = '0' + c2;
    } else {
      c2 = 'a' + (c2 - 10);
    }

    p_buf[16 - 2 - (i * 2) + 1] = c1;
    p_buf[16 - 2 - (i * 2) ] = c2;

    val >>= 8;
  }
}

static void
fault_reraise(void* p_pc,
              void* p_addr,
              int is_illegal,
              int is_write,
              int is_exec) {
  int ret;
  char hex_buf[16];
  char digit;

  static const char* p_msg = "FAULT: pc ";
  static const char* p_msg2 = ", addr ";
  static const char* p_msg3 = ", write ";
  static const char* p_msg4 = ", exec ";
  static const char* p_msg5 = ", illegal ";
  static const char* p_msg6 = "\n";

  ret = write(2, p_msg, strlen(p_msg));
  jit_safe_hex_convert(&hex_buf[0], p_pc);
  ret = write(2, hex_buf, sizeof(hex_buf));
  ret = write(2, p_msg2, strlen(p_msg2));
  jit_safe_hex_convert(&hex_buf[0], p_addr);
  ret = write(2, hex_buf, sizeof(hex_buf));
  ret = write(2, p_msg3, strlen(p_msg3));
  if (is_write == -1) {
    digit = '?';
  } else {
    digit = ('0' + is_write);
  }
  ret = write(2, &digit, 1);
  ret = write(2, p_msg4, strlen(p_msg4));
  if (is_exec == -1) {
    digit = '?';
  } else {
    digit = ('0' + is_exec);
  }
  ret = write(2, &digit, 1);
  ret = write(2, p_msg5, strlen(p_msg5));
  digit = ('0' + is_illegal);
  ret = write(2, &digit, 1);
  ret = write(2, p_msg6, strlen(p_msg6));

  (void) ret;

  os_fault_bail();
}

static void
jit_handle_fault(uintptr_t* p_host_pc,
                 uintptr_t host_fault_addr,
                 int is_illegal,
                 int is_exec,
                 int is_write,
                 uintptr_t host_context) {
  struct jit_struct* p_jit;
  int32_t addr_6502 = -1;
  uintptr_t new_pc = *p_host_pc;
  int is_inturbo = 0;

  void* p_fault_pc = (void*) *p_host_pc;
  void* p_fault_addr = (void*) host_fault_addr;

  /* Illegal instructions are not expected! */
  if (is_illegal) {
    fault_reraise(p_fault_pc, p_fault_addr, 1, 0, 0);
  }

  /* Fail unless the faulting instruction is in the JIT or inturbo region. */
  if ((p_fault_pc >= (void*) K_JIT_ADDR) &&
      (p_fault_pc < (void*) K_JIT_ADDR_END)) {
    /* JIT code. Continue. */
  } else if ((p_fault_pc >= (void*) K_INTURBO_ADDR) &&
             (p_fault_pc < (void*) K_INTURBO_ADDR_END)) {
    /* Inturbo code. Continue. */
    is_inturbo = 1;
  } else {
    fault_reraise(p_fault_pc, p_fault_addr, 0, is_write, is_exec);
  }

  /* Fault in instruction fetch would be bad! */
  if (is_exec == 1) {
    fault_reraise(p_fault_pc, p_fault_addr, 0, is_write, is_exec);
  }

  if (!is_inturbo) {
    p_jit = (struct jit_struct*) host_context;
  } else {
    /* Inturbo stores a pointer to JIT pointers in it's private member. */
    void** p_inturbo = (void**) host_context;
    void* p_jit_ptrs = *p_inturbo;
    p_jit = (struct jit_struct*) ((uint8_t*) p_jit_ptrs -
                                  K_JIT_CONTEXT_OFFSET_JIT_PTRS);
  }
  /* Sanity check it is really a jit struct. */
  if (p_jit->p_compile_callback != jit_compile) {
    fault_reraise(p_fault_pc, p_fault_addr, 0, is_write, is_exec);
  }

  if (!is_inturbo) {
    /* NOTE -- may call assert() which isn't async safe but faulting context is
     * raw asm, shouldn't be a disaster.
     */
    assert(((uintptr_t) p_fault_pc & (K_JIT_BYTES_PER_BYTE - 1)) != 0);
    addr_6502 = jit_metadata_get_6502_pc_from_host_pc(p_jit->p_metadata,
                                                      p_fault_pc);
  }

  /* Bail unless it's a clearly recognized fault. */
  if (!asm_jit_handle_fault(p_jit->p_asm,
                            &new_pc,
                            is_inturbo,
                            addr_6502,
                            p_fault_addr,
                            is_write)) {
    fault_reraise(p_fault_pc, p_fault_addr, 0, is_write, is_exec);
  }

  if (p_jit->log_fault) {
    /* Not the cleverest thing to do in a fault context, but it should be ok
     * because it's a fault in JIT code; it's also a debug option that needs to
     * be turned on.
     */
    log_do_log(k_log_jit,
               k_log_info,
               "JIT faulting at 6502 $%.4X pc 0x%"PRIx64" fault addr 0x%"PRIx64,
               addr_6502,
               (uint64_t) p_fault_pc,
               (uint64_t) p_fault_addr);
  }

  if ((p_jit->counter_num_faults % 10000) == 0) {
    /* We shouldn't call logging in the fault context (re-entrancy etc.) so set
     * a flag to take care of it later.
     */
    p_jit->do_fault_log = 1;
  }
  p_jit->counter_num_faults++;

  *p_host_pc = new_pc;
}

static void
jit_init(struct cpu_driver* p_cpu_driver) {
  struct interp_struct* p_interp;
  uint8_t* p_jit_base;
  struct util_buffer* p_temp_buf;
  void* p_no_code_mapping_addr;
  struct jit_metadata* p_metadata;
  uint32_t i;

  struct jit_struct* p_jit = (struct jit_struct*) p_cpu_driver;
  struct state_6502* p_state_6502 = p_cpu_driver->abi.p_state_6502;
  struct memory_access* p_memory_access =
      p_cpu_driver->p_extra->p_memory_access;
  struct timing_struct* p_timing = p_cpu_driver->p_extra->p_timing;
  struct bbc_options* p_options = p_cpu_driver->p_extra->p_options;
  struct debug_struct* p_debug = p_options->p_debug_object;
  int debug = debug_subsystem_active(p_debug);
  struct cpu_driver_funcs* p_funcs = p_cpu_driver->p_funcs;
  struct inturbo_struct* p_inturbo = NULL;

  p_jit->log_compile = util_has_option(p_options->p_log_flags, "jit:compile");
  p_jit->log_fault = util_has_option(p_options->p_log_flags, "jit:fault");
  p_jit->is_encoded_callbacks =
      !util_has_option(p_options->p_opt_flags, "jit:no-encoded-callback");
  p_funcs->get_opcode_maps(p_cpu_driver,
                           &p_jit->p_opcode_types,
                           &p_jit->p_opcode_modes,
                           &p_jit->p_opcode_mem,
                           &p_jit->p_opcode_cycles);

  p_funcs->destroy = jit_destroy;
  p_funcs->set_reset_callback = jit_set_reset_callback;
  p_funcs->set_memory_written_callback = jit_set_memory_written_callback;
  p_funcs->enter = jit_enter;
  p_funcs->apply_flags = jit_apply_flags;
  p_funcs->get_flags = jit_get_flags;
  p_funcs->get_exit_value = jit_get_exit_value;
  p_funcs->set_exit_value = jit_set_exit_value;
  p_funcs->memory_range_invalidate = jit_memory_range_invalidate;
  p_funcs->get_address_info = jit_get_address_info;
  p_funcs->get_custom_counters = jit_get_custom_counters;
  p_funcs->housekeeping_tick = jit_housekeeping_tick;

  p_jit->p_compile_callback = jit_compile;
  p_jit->p_jit_callback_context = p_memory_access->p_callback_obj;
  p_cpu_driver->abi.p_debug_asm = asm_debug_trampoline;
  p_cpu_driver->abi.p_interp_asm = asm_jit_interp_trampoline;

  /* The JIT mode uses an interpreter to handle complicated situations,
   * such as IRQs, hardware accesses, etc.
   */
  p_interp = (struct interp_struct*) cpu_driver_alloc(k_cpu_mode_interp,
                                                      0,
                                                      p_state_6502,
                                                      p_memory_access,
                                                      p_timing,
                                                      p_options);
  assert(((struct cpu_driver*) p_interp)->p_extra->type == k_cpu_mode_interp);
  cpu_driver_init((struct cpu_driver*) p_interp);
  p_jit->p_interp = p_interp;

  interp_set_instruction_callback(p_interp,
                                  jit_interp_instruction_callback,
                                  p_jit);

  /* The JIT mode uses an inturbo to handle opcodes that are self-modified
   * continually.
   */
  if (asm_inturbo_is_enabled()) {
    struct cpu_driver* p_inturbo_driver;
    p_inturbo = (struct inturbo_struct*) cpu_driver_alloc(k_cpu_mode_inturbo,
                                                          0,
                                                          p_state_6502,
                                                          p_memory_access,
                                                          p_timing,
                                                          p_options);
    p_inturbo_driver = (struct cpu_driver*) p_inturbo;
    assert(p_inturbo_driver->p_extra->type == k_cpu_mode_inturbo);
    inturbo_set_interp(p_inturbo, p_interp);
    /* Enable inturbo ret mode, which means we can call it to interpret a single
     * instruction and it will ret right back to us after every instruction.
     */
    inturbo_set_ret_mode(p_inturbo);
    inturbo_set_do_write_invalidation(p_inturbo, &p_jit->jit_ptrs[0]);
    /* RTI has to go through interp so as not to break apart code blocks
     * arbitrarily.
     */
    inturbo_set_use_interp_for_opcode(p_inturbo, 0x40);
    cpu_driver_init(p_inturbo_driver);
  }
  p_jit->p_inturbo = p_inturbo;

  p_jit->driver.abi.p_interp_callback = jit_enter_interp;
  p_jit->driver.abi.p_interp_object = p_jit;

  /* Little dance to avoid GCC 11 bug with -Werror=stringop-overflow. */
  if (g_p_jit_base == NULL) {
    g_p_jit_base = (void*) K_JIT_ADDR;
  }

  /* This is the mapping that holds the dynamically JIT'ed code.
   * Directly after creation, it will be read-write.
   */
  p_jit->p_mapping_jit = os_alloc_get_mapping(g_p_jit_base, K_JIT_SIZE);
  p_jit_base = os_alloc_get_mapping_addr(p_jit->p_mapping_jit);
  p_temp_buf = util_buffer_create();
  p_jit->p_temp_buf = p_temp_buf;
  util_buffer_setup(p_temp_buf, p_jit_base, K_JIT_SIZE);
  asm_fill_with_trap(p_temp_buf);

  p_jit->p_jit_base = p_jit_base;

  p_jit->p_mapping_no_code_ptr =
      os_alloc_get_mapping((void*) K_JIT_NO_CODE_JIT_PTR_PAGE, 4096);
  p_no_code_mapping_addr =
      os_alloc_get_mapping_addr(p_jit->p_mapping_no_code_ptr);

  /* Ah the horrors, a fault / SIGSEGV handler! This actually enables a ton of
   * optimizations by using faults for very uncommon conditions, such that the
   * fast path doesn't need certain checks.
   */
  os_fault_register_handler(jit_handle_fault);

  p_metadata = jit_metadata_create(p_jit_base,
                                   p_no_code_mapping_addr,
                                   ((uint8_t*) p_no_code_mapping_addr + 4),
                                   &p_jit->jit_ptrs[0]);
  p_jit->p_metadata = p_metadata;
  p_jit->p_code_blocks = jit_metadata_get_code_blocks(p_metadata);
  /* Set up the invalidation markers. jit_memory_range_invalidate() only
   * invalidates existing blocks, and here we're starting from a clean slate.
   */
  for (i = 0; i < k_6502_addr_space_size; ++i) {
    void* p_jit_ptr = jit_metadata_get_host_block_address(p_metadata, i);
    asm_jit_invalidate_code_at(p_jit_ptr);
  }

  /* Anything the specific asm driver (x64 or ARM64) needs to get its job
   * done. This includes setting up initial mapping permissions on the JIT
   * mapping. Any setup requiring writing to the code area must occur above
   * this call.
   */
  p_jit->p_asm = asm_jit_create(p_jit_base,
                                p_memory_access->memory_is_always_ram,
                                p_memory_access->p_callback_obj);
  p_cpu_driver->abi.p_util_private = asm_jit_get_private(p_jit->p_asm);

  p_jit->p_compiler = jit_compiler_create(
      p_jit->p_asm,
      p_timing,
      p_memory_access,
      p_jit->p_metadata,
      p_options,
      debug,
      p_jit->p_opcode_types,
      p_jit->p_opcode_modes,
      p_jit->p_opcode_mem,
      p_jit->p_opcode_cycles);

  /* NOTE: the JIT code space hasn't been set up with the invalidation markers.
   * Power-on reset has the responsibility of marking the entire address space
   * as invalidated.
   */
}

struct cpu_driver*
jit_create(struct cpu_driver_funcs* p_funcs) {
  struct cpu_driver* p_cpu_driver;
  size_t alignment;

  if (!asm_jit_is_enabled()) {
    return NULL;
  }

  asm_jit_test_preconditions();

  /* Align the structure to a multiple of the L1 DTLB bucket stride. This is
   * because the structure contains pointers read by JIT code and we want
   * deterministic performance.
   */
  alignment = (4096 * (64 / 4));
#if BEEBJIT_ASM_THUMB2
  /* No TLB on a Cortex-M33, and the padding would be 64KB of a small host's
   * memory.
   */
  alignment = 64;
#endif

  p_cpu_driver =
      (struct cpu_driver*) os_alloc_get_aligned(alignment,
                                                sizeof(struct jit_struct));
  (void) memset(p_cpu_driver, '\0', sizeof(struct jit_struct));

  p_funcs->init = jit_init;

  return p_cpu_driver;
}

#include "test-jit.c"
