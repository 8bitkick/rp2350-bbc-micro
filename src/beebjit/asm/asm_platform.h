#ifndef BEEBJIT_ASM_PLATFORM_H
#define BEEBJIT_ASM_PLATFORM_H

#if __APPLE__
#define ASM_SYM(x) _ ## x
#else
#define ASM_SYM(x) x
#endif

#if BEEBJIT_ASM_THUMB2
/* Thumb-2 (Cortex-M33, e.g. RP2350): no MMU, so JIT code lives in a fixed
 * region the host provides (os_alloc_get_mapping() asks it for these
 * addresses). On the RP2350 that is the bottom 4MB of the PSRAM through its
 * cached XIP window, then the no-code page, the backend's stub page and
 * inturbo's handlers, 0x410000 bytes in all; the test harnesses map the same.
 * 64 bytes of host code per 6502 byte: Thumb-2 is compact, and 4MB is what
 * fits.
 * (jit_metadata ORs JIT pointers with the base, so the no-code page at
 * K_JIT_ADDR_END must contain the base's bits: 0x11400000 does.)
 */
#define K_JIT_BYTES_SHIFT                  6
/* The host's choice (e.g. QEMU's mps2-an505 has no RAM at 0x11000000). It
 * must be a Thumb-2 modified immediate (the code adds it as one).
 */
#ifdef BEEBJIT_THUMB2_JIT_ADDR
#define K_JIT_ADDR                         BEEBJIT_THUMB2_JIT_ADDR
#else
#define K_JIT_ADDR                         0x11000000
#endif
/* Inturbo's 256 handlers (32KB) end the fixed region: after the JIT code,
 * the no-code page and the stub page.
 */
#define K_INTURBO_ADDR                     (K_JIT_ADDR + 0x408000)
/* Unused without an MMU (see BEEBJIT_NO_MMU). */
#define K_BBC_MEM_RAW_ADDR                 0x0f008000
#define K_ASM_TABLE_ADDR                   0x0
#elif defined(__x86_64__)
/* NOTE: this affects performance significantly.
 * Smaller is generally faster, which I believe is an L1 icache effect.
 * We can get to 6 on x64, which is 64 byte blocks, aligning nicely
 * with he L1 icache cacheline size. Currently unclear if that would be faster
 * than the current 7.
 */
#define K_JIT_BYTES_SHIFT                  7
#else
#define K_JIT_BYTES_SHIFT                  7
#endif

#if BEEBJIT_ASM_THUMB2
/* Addresses are above. */
#elif __APPLE__
#if defined(__x86_64__)
/* Apple macOS on x64. System mappings spill upwards from the 0x00400000 binary
 * placement we chose.
 * Place our mappings plenty higher to avoid collisions, but within +2GB jump
 * range.
 */
#define K_BBC_MEM_RAW_ADDR                 0x70008000
#define K_JIT_ADDR                         0x75000000
#define K_INTURBO_ADDR                     0x76000000
#define K_ASM_TABLE_ADDR                   0x77000000
#define K_JIT_TRAMPOLINES_ADDR             0x77800000
#else
/* Apple macOS on ARM64. 64-bit addresses strictly required. <4GB unmappable. */
#define K_BBC_MEM_RAW_ADDR                 0x700f008000
#define K_JIT_ADDR                         0x7ffc000000
#define K_INTURBO_ADDR                     0x7fff000000
#define K_ASM_TABLE_ADDR                   0x0          /* Unused om ARM64. */
#endif
#else
/* Linux and Windows. */
#define K_BBC_MEM_RAW_ADDR                 0x0f008000
#define K_JIT_ADDR                         0x06000000
#define K_INTURBO_ADDR                     0x07000000
#define K_ASM_TABLE_ADDR                   0x50000000
#define K_JIT_TRAMPOLINES_ADDR             0x80000000
#endif

#endif /* BEEBJIT_ASM_PLATFORM_H */
