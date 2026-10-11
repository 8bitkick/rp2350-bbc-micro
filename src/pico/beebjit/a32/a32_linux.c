/*
 * A minimal "C runtime" for running Cortex-M33 code as a 32-bit Linux process
 * on an Arm64 host that can run AArch32 user code (e.g. a Raspberry Pi 5,
 * Cortex-A76). The code is built with the same arm-none-eabi toolchain and
 * -mcpu=cortex-m33 flags as the RP2350 firmware, linked against newlib, and
 * runs natively: ARMv8-M Mainline Thumb-2 is a subset of A32/T32 user mode.
 *
 * Provides _start and the newlib system calls, using Linux EABI syscalls.
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/time.h>
#include <stdio.h>

#undef errno
extern int errno;

/* Linux ARM EABI syscall numbers. */
enum {
  k_nr_exit = 1,
  k_nr_read = 3,
  k_nr_write = 4,
  k_nr_open = 5,
  k_nr_close = 6,
  k_nr_lseek = 19,
  k_nr_getpid = 20,
  k_nr_kill = 37,
  k_nr_rt_sigaction = 174,
  k_nr_nanosleep = 162,
  k_nr_poll = 168,
  k_nr_exit_group = 248,
  k_nr_clock_gettime = 263,
  k_nr_mmap2 = 192,
  k_nr_cacheflush = 0x0f0002,
};

static long
a32_syscall6(long n, long a, long b, long c, long d, long e, long f) {
  register long r7 __asm__("r7") = n;
  register long r0 __asm__("r0") = a;
  register long r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c;
  register long r3 __asm__("r3") = d;
  register long r4 __asm__("r4") = e;
  register long r5 __asm__("r5") = f;
  __asm__ volatile("svc 0"
                   : "+r"(r0)
                   : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                   : "memory");
  return r0;
}

long
a32_syscall(long n, long a, long b, long c) {
  register long r7 __asm__("r7") = n;
  register long r0 __asm__("r0") = a;
  register long r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c;
  __asm__ volatile("svc 0"
                   : "+r"(r0)
                   : "r"(r7), "r"(r1), "r"(r2)
                   : "memory");
  return r0;
}

static int
a32_ret(long ret) {
  if ((ret < 0) && (ret > -4096)) {
    errno = (int) -ret;
    return -1;
  }
  return (int) ret;
}

/* Heap for newlib's malloc. Its size stands in for the RP2350's SRAM, so that
 * running out here is a hint that the firmware will too.
 */
#ifndef A32_HEAP_BYTES
#define A32_HEAP_BYTES (48u << 20)
#endif
static uint8_t s_heap[A32_HEAP_BYTES] __attribute__((aligned(16)));
static size_t s_heap_used;

void*
_sbrk(ptrdiff_t incr) {
  void* p_ret;
  if ((incr < 0) || ((s_heap_used + incr) > sizeof(s_heap))) {
    errno = ENOMEM;
    return (void*) -1;
  }
  p_ret = &s_heap[s_heap_used];
  s_heap_used += incr;
  return p_ret;
}

size_t
a32_heap_used(void) {
  return s_heap_used;
}

void
_exit(int status) {
  (void) a32_syscall(k_nr_exit_group, status, 0, 0);
  for (;;) {
    (void) a32_syscall(k_nr_exit, status, 0, 0);
  }
}

int
_write(int fd, const void* p_buf, size_t len) {
  return a32_ret(a32_syscall(k_nr_write, fd, (long) p_buf, len));
}

int
_read(int fd, void* p_buf, size_t len) {
  return a32_ret(a32_syscall(k_nr_read, fd, (long) p_buf, len));
}

int
_open(const char* p_name, int flags, int mode) {
  /* newlib's O_* values differ from Linux's. */
  long linux_flags = (flags & O_ACCMODE);
  if (flags & O_CREAT) {
    linux_flags |= 0100;
  }
  if (flags & O_EXCL) {
    linux_flags |= 0200;
  }
  if (flags & O_TRUNC) {
    linux_flags |= 01000;
  }
  if (flags & O_APPEND) {
    linux_flags |= 02000;
  }
  /* O_LARGEFILE: harmless, and lets lseek on big files behave. */
  linux_flags |= 0400000;
  return a32_ret(a32_syscall(k_nr_open, (long) p_name, linux_flags, mode));
}

int
_close(int fd) {
  return a32_ret(a32_syscall(k_nr_close, fd, 0, 0));
}

off_t
_lseek(int fd, off_t pos, int whence) {
  return a32_ret(a32_syscall(k_nr_lseek, fd, pos, whence));
}

int
_fstat(int fd, struct stat* p_stat) {
  (void) memset(p_stat, '\0', sizeof(*p_stat));
  p_stat->st_mode = (fd <= 2) ? S_IFCHR : S_IFREG;
  return 0;
}

int
_isatty(int fd) {
  return (fd <= 2);
}

int
_getpid(void) {
  return a32_syscall(k_nr_getpid, 0, 0, 0);
}

int
_kill(int pid, int sig) {
  return a32_ret(a32_syscall(k_nr_kill, pid, sig, 0));
}

int
_unlink(const char* p_name) {
  (void) p_name;
  errno = ENOSYS;
  return -1;
}

int
_link(const char* p_old, const char* p_new) {
  (void) p_old;
  (void) p_new;
  errno = ENOSYS;
  return -1;
}

int
_stat(const char* p_name, struct stat* p_stat) {
  (void) p_name;
  (void) p_stat;
  errno = ENOSYS;
  return -1;
}

uint64_t
a32_time_us(void) {
  struct {
    int32_t tv_sec;
    int32_t tv_nsec;
  } ts;
  /* CLOCK_MONOTONIC = 1. */
  (void) a32_syscall(k_nr_clock_gettime, 1, (long) &ts, 0);
  return ((uint64_t) ts.tv_sec * 1000000u) + (ts.tv_nsec / 1000);
}

int
_gettimeofday(struct timeval* p_tv, void* p_tz) {
  uint64_t us = a32_time_us();
  (void) p_tz;
  p_tv->tv_sec = (us / 1000000u);
  p_tv->tv_usec = (us % 1000000u);
  return 0;
}

clock_t
_times(struct tms* p_tms) {
  (void) memset(p_tms, '\0', sizeof(*p_tms));
  return (clock_t) -1;
}

void
a32_sleep_us(uint64_t us) {
  struct {
    int32_t tv_sec;
    int32_t tv_nsec;
  } ts;
  ts.tv_sec = (us / 1000000u);
  ts.tv_nsec = ((us % 1000000u) * 1000u);
  (void) a32_syscall(k_nr_nanosleep, (long) &ts, 0, 0);
}

int
a32_poll_readable(int fd) {
  struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
  } pfd;
  pfd.fd = fd;
  pfd.events = 1; /* POLLIN */
  pfd.revents = 0;
  return (a32_syscall(k_nr_poll, (long) &pfd, 1, 0) > 0) &&
         (pfd.revents & 1);
}

/* No crti/crtn with -nostartfiles. */
void _init(void) {}
void _fini(void) {}

extern void __libc_init_array(void);
int main(int argc, char** argv);

/* A crash report for JIT debugging: SIGILL / SIGSEGV / SIGBUS print the
 * registers (from the ARM ucontext: r0 at offset 32, then r1-r12, sp, lr,
 * pc, cpsr)
 * and the code around the PC, then exit.
 */
static void
a32_fault_handler(int sig, void* p_info, void* p_ucontext) {
  uint32_t* p_regs = (uint32_t*) ((uint8_t*) p_ucontext + 32);
  uint32_t pc = p_regs[15];
  uint32_t i;
  (void) p_info;
  (void) printf("\nFAULT sig %d pc %08lx lr %08lx sp %08lx cpsr %08lx\n",
                sig, (unsigned long) pc, (unsigned long) p_regs[14],
                (unsigned long) p_regs[13], (unsigned long) p_regs[16]);
  for (i = 0; i < 13; ++i) {
    (void) printf("r%lu=%08lx%s", (unsigned long) i, (unsigned long) p_regs[i],
                  ((i % 7) == 6) ? "\n" : " ");
  }
  (void) printf("\ncode:");
  for (i = 0; i < 16; ++i) {
    (void) printf(" %04x", (unsigned) ((uint16_t*) ((pc & ~1u) - 16))[i]);
  }
  (void) printf("\n");
  fflush(stdout);
  (void) a32_syscall(k_nr_exit_group, 99, 0, 0);
}

static void
a32_install_fault_handler(void) {
  /* struct kernel_sigaction: handler, flags (SA_SIGINFO), restorer, mask. */
  static uint32_t s_act[4];
  int sigs[3] = { 4, 7, 11 };
  uint32_t i;
  s_act[0] = (uint32_t) (uintptr_t) a32_fault_handler;
  s_act[1] = 4;
  for (i = 0; i < 3; ++i) {
    (void) a32_syscall6(k_nr_rt_sigaction, sigs[i], (long) s_act, 0, 8, 0, 0);
  }
}

void __attribute__((used))
a32_start_c(long* p_stack) {
  int argc = (int) p_stack[0];
  char** argv = (char**) &p_stack[1];
  __libc_init_array();
  a32_install_fault_handler();
  exit(main(argc, argv));
}

/* The kernel enters here (in Thumb state: the ELF entry has bit 0 set) with
 * sp pointing at argc.
 */
__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start, %function\n"
    ".thumb_func\n"
    "_start:\n"
    "  mov r0, sp\n"
    "  bic r1, r0, #7\n"
    "  mov sp, r1\n"
    "  bl a32_start_c\n"
    "  b .\n");

/* ---- The test harness's host interface (../harness/harness_host.h) ---- */

#include "../harness/harness_host.h"

/* The Presto's big region: its 8MB of PSRAM less the JIT's fixed region
 * (0x410000), so that running out of it shows up here first.
 */
static uint8_t s_big_region[(8u << 20) - 0x410000u] __attribute__((aligned(16)));

uint64_t
harness_host_time_us(void) {
  return a32_time_us();
}

void
harness_host_sleep_us(uint64_t us) {
  a32_sleep_us(us);
}

int
harness_host_read_stdin(uint8_t* p_byte) {
  if (!a32_poll_readable(0)) {
    return 0;
  }
  return (a32_syscall(k_nr_read, 0, (long) p_byte, 1) == 1);
}

void
harness_host_write_stdout(uint8_t byte) {
  (void) a32_syscall(k_nr_write, 1, (long) &byte, 1);
}

size_t
harness_host_heap_used(void) {
  return s_heap_used;
}

size_t
harness_host_big_region(uint8_t** p_base) {
  *p_base = s_big_region;
  return sizeof(s_big_region);
}

/* The thumb2 JIT's fixed region: its code, then the no-code and stub pages,
 * 0x11000000 up to 0x11402000 (see asm/asm_platform.h), mapped read / write /
 * exec once. The mapping ends on a 64K boundary: the Pi 5's kernel has 16K
 * pages.
 */
#define A32_JIT_REGION_START 0x11000000u
#define A32_JIT_REGION_END   0x11410000u

void*
harness_host_fixed_region(void* p_addr, size_t size) {
  static int s_mapped;
  uintptr_t start = (uintptr_t) p_addr;
  if ((start < A32_JIT_REGION_START) || ((start + size) > A32_JIT_REGION_END)) {
    return NULL;
  }
  if (!s_mapped) {
    /* PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS |
     * MAP_FIXED.
     */
    long ret = a32_syscall6(k_nr_mmap2,
                            (long) A32_JIT_REGION_START,
                            (long) (A32_JIT_REGION_END - A32_JIT_REGION_START),
                            7,
                            (0x02 | 0x20 | 0x10),
                            -1,
                            0);
    if (ret != (long) A32_JIT_REGION_START) {
      (void) printf("#jit region mmap failed: %ld\n", ret);
      return NULL;
    }
    s_mapped = 1;
  }
  return p_addr;
}

void
harness_host_flush_code(void* p, size_t length) {
  (void) a32_syscall(k_nr_cacheflush,
                     (long) p,
                     (long) ((uint8_t*) p + length),
                     0);
}

int
harness_host_needs_code_flush(void) {
  /* The A76 has an instruction cache that doesn't see data writes. */
  return 1;
}

uint64_t
harness_host_cycles(void) {
  return 0;
}

const char*
harness_host_cycles_unit(void) {
  return NULL;
}

void
harness_host_profile(int on) {
  (void) on;
}

void
harness_host_profile_save(const char* p_file_name) {
  (void) p_file_name;
}
