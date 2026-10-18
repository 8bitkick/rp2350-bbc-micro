#ifndef BEEBJIT_ASM_THUMB2_DEFS_H
#define BEEBJIT_ASM_THUMB2_DEFS_H

/* Register use by the thumb2 JIT backend (Cortex-M33). The low registers that
 * cbz / cbnz and 16-bit encodings can use go to the 6502 registers and the JIT
 * scratch register.
 *
 * 6502 state while in JIT code:
 *   A, X, Y, S: 0..255.
 *   N, Z: the host N and Z flags.
 *   C, V: 0 / 1 in REG_CF / REG_OF, and also the host C / V flags after an
 *         instruction that sets them, until something trashes the host flags
 *         (see jit_optimizer.c: the backend must keep host C / V intact
 *         except in NZ flag uops and CLD / CLI / SED / SEI / BIT / PLA).
 *   I, D: in two bytes just before the 6502 memory (the host keeps 16 spare
 *         bytes there): D at REG_MEM - 4 (0 or 0x08), I at REG_MEM - 3 (0 or
 *         0x04), as their bits in the 6502 P register.
 *   PC: implicit in the code address; in REG_JIT_SCRATCH on the way to the
 *       interpreter.
 * lr holds the compile trampoline, so that invalidated code, "blx lr", calls
 * the compiler. Anything that clobbers lr in JIT code restores it.
 */
#define REG_A           0
#define REG_X           1
#define REG_Y           2
#define REG_S           3
#define REG_CF          4
#define REG_OF          5
#define REG_VALUE       6
#define REG_JIT_SCRATCH 7
#define REG_SCRATCH1    8
#define REG_SCRATCH2    9
#define REG_ADDR_BASE   9
#define REG_MEM         10
#define REG_COUNTDOWN   11
#define REG_ADDR        12
#define REG_COMPILE     14

#define K_T2_MEM_D_FLAG 4
#define K_T2_MEM_I_FLAG 3

/* "blx lr": the invalidated code marker. */
#define K_T2_INVALIDATED 0x47F0

/* Inturbo (asm_inturbo_thumb2.c): one 6502 instruction at a time, for the
 * JIT's dynamic opcodes and sub-instruction entries. A handler per opcode, in
 * the JIT state model above plus:
 *   REG_INTURBO_PC: the 6502 PC (16 bits).
 *   REG_INTURBO_EA: the effective address (or the immediate operand).
 *   REG_INTURBO_BASE: the base address before indexing, or a branch target.
 *   REG_INTURBO_CD: the countdown after this instruction, until committed.
 * lr is free in a handler: it ends by jumping to the JIT code for the new PC,
 * through a stub that sets lr back to the compile stub.
 */
#define REG_INTURBO_PC   REG_ADDR
#define REG_INTURBO_EA   REG_SCRATCH1
#define REG_INTURBO_BASE REG_SCRATCH2
#define REG_INTURBO_CD   14

/* The stub page: each stub at a fixed offset, so that code generated before
 * the stubs are (inturbo's handlers, at JIT creation) can call them.
 */
#define K_T2_STUB_ADDR             (K_JIT_ADDR_END + 0x1000)
#define K_T2_STUB_SIZE             0x1000
#define K_T2_STUB_COMPILE          0x000
#define K_T2_STUB_PHP              0x180
#define K_T2_STUB_PLP              0x200
#define K_T2_STUB_PULL16           0x280
#define K_T2_STUB_EXIT             0x300
#define K_T2_STUB_INTERP           0x320
#define K_T2_STUB_FLUSH_INV        0x4A0
#define K_T2_STUB_INTERP_INLINE    0x520
#define K_T2_STUB_ENTER            0x540
#define K_T2_STUB_INTURBO          0x640
#define K_T2_STUB_INTURBO_EXIT     0x680
#define K_T2_STUB_INTURBO_INTERP   0x6C0
#define K_T2_STUB_WRITE_FLAGS      0x840
#define K_T2_STUB_INTURBO_WRITE_FLAGS 0x8C0
#define K_T2_STUB_PUSH16           0x940
#define K_T2_STUB_JMP_SCRATCH      0x9C0
#define K_T2_STUB_RTS              0x9E0
#define K_T2_STUB_END              0xA40

#include <stddef.h>
#include <stdint.h>

struct state_6502;

/* Backend state referenced by generated code by absolute address. */
struct t2_globals {
  struct state_6502* p_state_6502;
  void* p_context;
  uint32_t* p_jit_ptrs;
  uint8_t* p_mem;
  /* The memory callback object (bbc_struct), for encoded callbacks. */
  void* p_bbc;
  void** p_compile_callback;
  void** p_interp_callback;
  void** p_interp_object;
  /* Generated stubs (Thumb addresses, bit 0 set where they're called). */
  uint8_t* p_stub_enter;
  uint8_t* p_stub_compile;
  uint8_t* p_stub_interp;
  uint8_t* p_stub_exit;
  /* Helpers called with bl from JIT code (they restore lr on the way out). */
  uint8_t* p_stub_php;
  uint8_t* p_stub_plp;
  uint8_t* p_stub_pull16;
  /* JSR / BRK: push the 16-bit value in REG_SCRATCH1 (high byte first). */
  uint8_t* p_stub_push16;
  uint8_t* p_stub_jmp_scratch;
  uint8_t* p_stub_rts;
  /* Exit to the interpreter: "bl" here, followed by the 6502 PC as a
   * halfword.
   */
  uint8_t* p_stub_interp_inline;
  /* Harness only (p_flush_code set): after a write invalidation, flush the
   * code at REG_JIT_SCRATCH from the instruction cache.
   */
  uint8_t* p_stub_flush_inv;
  /* Into inturbo: b here with the 6502 PC in REG_INTURBO_PC; it runs the
   * handler for the opcode there.
   */
  uint8_t* p_stub_inturbo;
  /* Out of an inturbo handler: b here with the new PC in REG_INTURBO_PC. */
  uint8_t* p_stub_inturbo_exit;
  /* From an inturbo handler to the interpreter: b here with the PC in
   * REG_JIT_SCRATCH. Through inturbo's own interp callback and object.
   */
  uint8_t* p_stub_inturbo_interp;
  void** p_inturbo_interp_object;
  void** p_inturbo_interp_callback;
  /* A store to a page with flags (bbc.h: g_bbc_page_flags): bl here with
   * the flags in REG_JIT_SCRATCH and the address in REG_ADDR (the inturbo
   * one: in REG_INTURBO_EA, lr free). Marks the picture changed for a screen
   * page; invalidates the JIT code for a code page.
   */
  uint8_t* p_stub_write_flags;
  uint8_t* p_stub_inturbo_write_flags;
  /* Set by a host whose instruction fetch doesn't see data writes (the a32
   * test harness on a Cortex-A); NULL on the RP2350.
   */
  void (*p_flush_code)(void* p, size_t length);
};

extern struct t2_globals g_t2;

/* The stub addresses (fixed) into g_t2; the stubs themselves are generated at
 * the first entry into JIT code.
 */
void t2_layout_stubs(void);

#endif /* BEEBJIT_ASM_THUMB2_DEFS_H */
