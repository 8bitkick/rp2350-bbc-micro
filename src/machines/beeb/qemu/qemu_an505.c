/*
 * Bare-metal host for the test harness on QEMU's mps2-an505 (Cortex-M33):
 * reset handler and vector table, semihosting for the command line, files and
 * console (newlib's librdimon), and an instruction counter.
 *
 * Counting: run QEMU with -icount shift=0, so that virtual time advances 1 ns
 * per instruction. SysTick counts that virtual time; calibrate() measures
 * how many instructions a tick is, so harness_host_cycles() returns executed
 * instructions (to within one tick). That's a proxy for M33 cycles, not a
 * measurement of them: loads, taken branches and table branches cost more.
 * The SysTick interrupt also samples the PC for profiling (-profile).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "../harness/harness_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv);
extern void initialise_monitor_handles(void);
extern void __libc_init_array(void);

extern uint32_t __bss_start__;
extern uint32_t __bss_end__;
extern uint32_t __stack_top;
extern uint8_t __big_start;
extern uint8_t __big_end;
extern uint8_t end;

/* ---- Semihosting ---- */

static int
semihost(int op, void* p_arg) {
  register int r0 __asm__("r0") = op;
  register void* r1 __asm__("r1") = p_arg;
  __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}

enum {
  k_sys_get_cmdline = 0x15,
};

static char s_cmdline[1024];
static char* s_argv[64];

/* Split the command line (from QEMU's -append) on spaces, honouring '...'
 * and "..." quotes.
 */
static int
get_args(void) {
  struct {
    char* p_buf;
    int len;
  } block;
  int argc = 0;
  char* p;
  char* p_out;

  block.p_buf = s_cmdline;
  block.len = (sizeof(s_cmdline) - 1);
  if (semihost(k_sys_get_cmdline, &block) != 0) {
    s_argv[0] = "harness";
    return 1;
  }
  s_cmdline[block.len] = '\0';
  p = s_cmdline;
  while (*p != '\0') {
    char quote = 0;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    if (argc == (int) (sizeof(s_argv) / sizeof(s_argv[0]) - 1)) {
      break;
    }
    s_argv[argc++] = p_out = p;
    while (*p != '\0') {
      if (quote) {
        if (*p == quote) {
          quote = 0;
          p++;
          continue;
        }
      } else if ((*p == '\'') || (*p == '"')) {
        quote = *p++;
        continue;
      } else if (*p == ' ') {
        p++;
        break;
      }
      *p_out++ = *p++;
    }
    *p_out = '\0';
  }
  s_argv[argc] = NULL;
  return argc;
}

/* ---- SysTick: instruction counter and PC sampler ---- */

#define SYST_CSR (*(volatile uint32_t*) 0xE000E010)
#define SYST_RVR (*(volatile uint32_t*) 0xE000E014)
#define SYST_CVR (*(volatile uint32_t*) 0xE000E018)
#define CPACR (*(volatile uint32_t*) 0xE000ED88)

/* A SysTick interrupt every k_systick_period ticks (each tick is ~50
 * instructions under -icount shift=0): it counts wraps, and samples the
 * interrupted PC when profiling.
 */
enum {
  k_systick_period = 200,
  k_max_samples = (1u << 20),
};

static volatile uint32_t s_wraps;
static uint32_t s_insns_per_tick_x256;
static volatile int s_profiling;
static uint32_t s_num_samples;
static uint32_t s_samples[k_max_samples] __attribute__((section(".samples")));

void
systick_c(uint32_t pc) {
  s_wraps++;
  if (s_profiling && (s_num_samples < k_max_samples)) {
    s_samples[s_num_samples++] = pc;
  }
}

/* The thread runs on MSP; the stacked PC is the 7th word of the frame. */
static void __attribute__((naked))
systick_handler(void) {
  __asm__ volatile(
      "mrs r0, msp\n"
      "ldr r0, [r0, #24]\n"
      "b systick_c\n");
}

static uint64_t
ticks_now(void) {
  uint32_t wraps;
  uint32_t cvr;
  do {
    wraps = s_wraps;
    cvr = SYST_CVR;
  } while (wraps != s_wraps);
  return ((uint64_t) wraps * k_systick_period) +
         (k_systick_period - 1 - cvr);
}

static void
calibrate(void) {
  uint64_t t0;
  uint64_t t1;
  uint32_t n = 4000000;
  SYST_RVR = (k_systick_period - 1);
  SYST_CVR = 0;
  /* Enable, interrupt, processor clock. */
  SYST_CSR = 7;
  t0 = ticks_now();
  /* 2 instructions an iteration. */
  __asm__ volatile("1: subs %0, %0, #1\n bne 1b\n" : "+r"(n) : : "cc");
  t1 = ticks_now();
  if (t1 > t0) {
    s_insns_per_tick_x256 = (uint32_t) ((8000000ull * 256) / (t1 - t0));
  }
}

void
harness_host_profile(int on) {
  s_profiling = on;
}

void
harness_host_profile_save(const char* p_file_name) {
  FILE* p_file;
  if (s_num_samples == 0) {
    return;
  }
  p_file = fopen(p_file_name, "wb");
  if (p_file == NULL) {
    (void) printf("can't write %s\n", p_file_name);
    return;
  }
  (void) fwrite(s_samples, sizeof(uint32_t), s_num_samples, p_file);
  (void) fclose(p_file);
  (void) printf("%lu PC samples (one per %lu instructions) in %s\n",
                (unsigned long) s_num_samples,
                (unsigned long) ((k_systick_period * s_insns_per_tick_x256) >>
                                 8),
                p_file_name);
}

/* ---- harness_host.h ---- */

uint64_t
harness_host_time_us(void) {
  return 0;
}

void
harness_host_sleep_us(uint64_t us) {
  (void) us;
}

int
harness_host_read_stdin(uint8_t* p_byte) {
  (void) p_byte;
  return 0;
}

void
harness_host_write_stdout(uint8_t byte) {
  (void) putchar(byte);
  if (byte == '\n') {
    (void) fflush(stdout);
  }
}

size_t
harness_host_heap_used(void) {
  extern void* _sbrk(int incr);
  return ((uint8_t*) _sbrk(0) - &end);
}

size_t
harness_host_big_region(uint8_t** p_base) {
  *p_base = &__big_start;
  return (&__big_end - &__big_start);
}

/* The thumb2 JIT's fixed region, the start of BIGRAM (see an505.ld). */
void*
harness_host_fixed_region(void* p_addr, size_t size) {
  uintptr_t start = (uintptr_t) p_addr;
  if ((start < 0x80000000u) || ((start + size) > 0x80410000u)) {
    return NULL;
  }
  return p_addr;
}

void
harness_host_flush_code(void* p, size_t length) {
  (void) p;
  (void) length;
}

int
harness_host_needs_code_flush(void) {
  /* QEMU handles self-modifying code itself. */
  return 0;
}

uint64_t
harness_host_cycles(void) {
  if (s_insns_per_tick_x256 == 0) {
    return 0;
  }
  return ((ticks_now() * s_insns_per_tick_x256) >> 8);
}

const char*
harness_host_cycles_unit(void) {
  return (s_insns_per_tick_x256 != 0) ? "instructions" : NULL;
}

/* ---- Reset ---- */

void __attribute__((noreturn))
reset_handler(void) {
  uint32_t* p;
  int argc;
  /* FPU on (the code is built for armv8-m.main+fp). */
  CPACR |= (0xFu << 20);
  __asm__ volatile("dsb\n isb\n");
  /* QEMU's ELF loader fills .bss with zeros, but don't rely on it. */
  for (p = &__bss_start__; p < &__bss_end__; ++p) {
    *p = 0;
  }
  initialise_monitor_handles();
  __libc_init_array();
  calibrate();
  if (s_insns_per_tick_x256 != 0) {
    (void) printf("instruction counter: %.2f instructions per SysTick "
                  "(run QEMU with -icount shift=0)\n",
                  (s_insns_per_tick_x256 / 256.0));
  }
  argc = get_args();
  exit(main(argc, s_argv));
}
/* Fault report: the exception number, the fault status registers and the
 * stacked registers (for JIT debugging).
 */
void __attribute__((used))
fault_report(uint32_t* p_frame, uint32_t exc_return) {
  uint32_t ipsr;
  uint32_t i;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  (void) printf("fault: exception %lu CFSR %08lx HFSR %08lx MMFAR %08lx "
                "BFAR %08lx\n",
                (unsigned long) (ipsr & 0x1FF),
                (unsigned long) *(volatile uint32_t*) 0xE000ED28,
                (unsigned long) *(volatile uint32_t*) 0xE000ED2C,
                (unsigned long) *(volatile uint32_t*) 0xE000ED34,
                (unsigned long) *(volatile uint32_t*) 0xE000ED38);
  (void) printf("r0 %08lx r1 %08lx r2 %08lx r3 %08lx r12 %08lx lr %08lx "
                "pc %08lx xpsr %08lx exc_return %08lx\n",
                (unsigned long) p_frame[0], (unsigned long) p_frame[1],
                (unsigned long) p_frame[2], (unsigned long) p_frame[3],
                (unsigned long) p_frame[4], (unsigned long) p_frame[5],
                (unsigned long) p_frame[6], (unsigned long) p_frame[7],
                (unsigned long) exc_return);
  if ((p_frame[6] >= 0x10000000u) || (p_frame[6] < 0x90000000u)) {
    (void) printf("code:");
    for (i = 0; i < 16; ++i) {
      (void) printf(" %04x",
                    (unsigned) ((uint16_t*) ((p_frame[6] & ~1u) - 16))[i]);
    }
    (void) printf("\n");
  }
  exit(99);
}

static void __attribute__((naked))
fault_handler(void) {
  __asm__ volatile(
      "tst lr, #4\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "mov r1, lr\n"
      "b fault_report\n");
}

/* No crti/crtn with -nostartfiles. */
void _init(void) {}
void _fini(void) {}

__attribute__((section(".vectors"), used))
static void (* const s_vectors[16])(void) = {
  (void (*)(void)) &__stack_top,
  reset_handler,
  fault_handler,  /* NMI */
  fault_handler,  /* HardFault */
  fault_handler,  /* MemManage */
  fault_handler,  /* BusFault */
  fault_handler,  /* UsageFault */
  fault_handler,  /* SecureFault */
  0, 0, 0,
  fault_handler,  /* SVCall */
  fault_handler,  /* DebugMonitor */
  0,
  fault_handler,  /* PendSV */
  systick_handler,
};
