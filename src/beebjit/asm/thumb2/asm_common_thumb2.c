/* Thumb-2 (Cortex-M33) backend: instruction emitters shared by the JIT.
 * Register use and conventions are in thumb2_defs.h.
 *
 * Part of the RP2350 port of beebjit; GPL v3, as beebjit.
 */
#include "../asm_common.h"

#include "../../util.h"
#include "thumb2_defs.h"
#include "thumb2_emit.h"

struct t2_globals g_t2;

void
asm_fill_with_trap(struct util_buffer* p_buf) {
  size_t pos = util_buffer_get_pos(p_buf);
  size_t length = (util_buffer_get_length(p_buf) - pos);
  assert((pos % 2) == 0);
  while (length >= 2) {
    t2_udf(p_buf, 0);
    length -= 2;
  }
}

void
asm_emit_instruction_REAL_NOP(struct util_buffer* p_buf) {
  t2_nop(p_buf);
}

void
asm_emit_instruction_TRAP(struct util_buffer* p_buf) {
  t2_udf(p_buf, 1);
}

void
asm_emit_instruction_ILLEGAL(struct util_buffer* p_buf) {
  t2_udf(p_buf, 2);
}

/* BIT: N = bit 7 of the value, V = bit 6, Z = (A & value) == 0. Host C is
 * preserved; host V is set to match REG_OF.
 */
void
asm_emit_instruction_BIT_value(struct util_buffer* p_buf) {
  t2_ubfx(p_buf, REG_OF, REG_VALUE, 6, 1);
  t2_dp_reg(p_buf, k_t2_op_and, 1, REG_JIT_SCRATCH, REG_A, REG_VALUE,
            k_t2_lsl, 0);
  t2_mrs_apsr(p_buf, REG_JIT_SCRATCH);
  t2_shift(p_buf, k_t2_lsr, 0, REG_SCRATCH1, REG_VALUE, 7);
  t2_bfi(p_buf, REG_JIT_SCRATCH, REG_SCRATCH1, 31, 1);
  t2_bfi(p_buf, REG_JIT_SCRATCH, REG_OF, 28, 1);
  t2_msr_apsr(p_buf, REG_JIT_SCRATCH);
}

void
asm_emit_instruction_CLC(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_CF, 0);
}

void
asm_emit_instruction_SEC(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_CF, 1);
}

void
asm_emit_instruction_CLV(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_OF, 0);
}

/* Set the D or I flag byte before the 6502 memory. */
static void
t2_emit_flag_byte(struct util_buffer* p_buf, uint32_t offset, uint32_t value) {
  t2_mov32(p_buf, REG_JIT_SCRATCH, value);
  t2_ldst_neg(p_buf, 0, 0, REG_JIT_SCRATCH, REG_MEM, offset);
}

void
asm_emit_instruction_CLD(struct util_buffer* p_buf) {
  t2_emit_flag_byte(p_buf, K_T2_MEM_D_FLAG, 0);
}

void
asm_emit_instruction_SED(struct util_buffer* p_buf) {
  t2_emit_flag_byte(p_buf, K_T2_MEM_D_FLAG, 0x08);
}

void
asm_emit_instruction_CLI(struct util_buffer* p_buf) {
  t2_emit_flag_byte(p_buf, K_T2_MEM_I_FLAG, 0);
}

void
asm_emit_instruction_SEI(struct util_buffer* p_buf) {
  t2_emit_flag_byte(p_buf, K_T2_MEM_I_FLAG, 0x04);
}

/* reg = (reg + delta) & 0xFF, flags untouched. */
static void
t2_emit_inc8(struct util_buffer* p_buf, uint32_t reg, int delta) {
  if (delta >= 0) {
    t2_addw(p_buf, reg, reg, delta);
  } else {
    t2_subw(p_buf, reg, reg, -delta);
  }
  t2_dp_imm(p_buf, k_t2_op_and, 0, reg, reg, 0xFF);
}

void
asm_emit_instruction_DEX(struct util_buffer* p_buf) {
  t2_emit_inc8(p_buf, REG_X, -1);
}

void
asm_emit_instruction_DEY(struct util_buffer* p_buf) {
  t2_emit_inc8(p_buf, REG_Y, -1);
}

void
asm_emit_instruction_INX(struct util_buffer* p_buf) {
  t2_emit_inc8(p_buf, REG_X, 1);
}

void
asm_emit_instruction_INY(struct util_buffer* p_buf) {
  t2_emit_inc8(p_buf, REG_Y, 1);
}

/* Push / pull a byte register on the 6502 stack (page 1). */
void
t2_emit_push(struct util_buffer* p_buf, uint32_t reg) {
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_MEM, REG_S,
            k_t2_lsl, 0);
  t2_strb(p_buf, reg, REG_JIT_SCRATCH, 0x100);
  t2_emit_inc8(p_buf, REG_S, -1);
}

void
t2_emit_pull(struct util_buffer* p_buf, uint32_t reg) {
  t2_emit_inc8(p_buf, REG_S, 1);
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_MEM, REG_S,
            k_t2_lsl, 0);
  t2_ldrb(p_buf, reg, REG_JIT_SCRATCH, 0x100);
}

/* REG_SCRATCH1 = the 6502 P register (with bit 5 set, B clear). Flags
 * untouched.
 */
void
t2_emit_flags_to_scratch1(struct util_buffer* p_buf) {
  t2_mrs_apsr(p_buf, REG_JIT_SCRATCH);
  t2_ldst_neg(p_buf, 1, 0, REG_SCRATCH1, REG_MEM, K_T2_MEM_D_FLAG);
  t2_ldst_neg(p_buf, 1, 0, REG_SCRATCH2, REG_MEM, K_T2_MEM_I_FLAG);
  t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2,
            k_t2_lsl, 0);
  t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_SCRATCH1, REG_SCRATCH1, 0x20);
  /* Host N (bit 31) -> bit 1, Z (bit 30) -> bit 0. */
  t2_shift(p_buf, k_t2_lsr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 30);
  t2_bfi(p_buf, REG_SCRATCH1, REG_JIT_SCRATCH, 1, 1);
  t2_shift(p_buf, k_t2_lsr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
  t2_bfi(p_buf, REG_SCRATCH1, REG_JIT_SCRATCH, 7, 1);
  t2_bfi(p_buf, REG_SCRATCH1, REG_CF, 0, 1);
  t2_bfi(p_buf, REG_SCRATCH1, REG_OF, 6, 1);
}

/* The 6502 P register in REG_SCRATCH1 -> I / D, REG_CF, REG_OF and the host
 * N, Z, C, V flags.
 */
void
t2_emit_flags_from_scratch1(struct util_buffer* p_buf) {
  t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_SCRATCH1, 0x08);
  t2_ldst_neg(p_buf, 0, 0, REG_JIT_SCRATCH, REG_MEM, K_T2_MEM_D_FLAG);
  t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_SCRATCH1, 0x04);
  t2_ldst_neg(p_buf, 0, 0, REG_JIT_SCRATCH, REG_MEM, K_T2_MEM_I_FLAG);
  t2_ubfx(p_buf, REG_CF, REG_SCRATCH1, 0, 1);
  t2_ubfx(p_buf, REG_OF, REG_SCRATCH1, 6, 1);
  /* N: bit 7 -> 31. */
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_SCRATCH1, 24);
  t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
            0x80000000);
  /* Z: bit 1 -> 30. C -> 29, V -> 28. */
  t2_ubfx(p_buf, REG_SCRATCH2, REG_SCRATCH1, 1, 1);
  t2_bfi(p_buf, REG_JIT_SCRATCH, REG_SCRATCH2, 30, 1);
  t2_bfi(p_buf, REG_JIT_SCRATCH, REG_CF, 29, 1);
  t2_bfi(p_buf, REG_JIT_SCRATCH, REG_OF, 28, 1);
  t2_msr_apsr(p_buf, REG_JIT_SCRATCH);
}

void
asm_emit_instruction_PHA(struct util_buffer* p_buf) {
  t2_emit_push(p_buf, REG_A);
}

/* PHP and PLP don't fit a 1-byte opcode's 64 bytes of JIT space inline, so
 * they're helpers in the stub page (see asm_jit_thumb2.c), called with bl.
 */
void
t2_emit_php_body(struct util_buffer* p_buf) {
  t2_emit_flags_to_scratch1(p_buf);
  /* PHP pushes with B set. */
  t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_SCRATCH1, REG_SCRATCH1, 0x10);
  t2_emit_push(p_buf, REG_SCRATCH1);
}

void
t2_emit_plp_body(struct util_buffer* p_buf) {
  t2_emit_pull(p_buf, REG_SCRATCH1);
  t2_emit_flags_from_scratch1(p_buf);
}

void
asm_emit_instruction_PHP(struct util_buffer* p_buf) {
  t2_bl(p_buf, g_t2.p_stub_php);
}

void
asm_emit_instruction_PLA(struct util_buffer* p_buf) {
  t2_emit_pull(p_buf, REG_A);
}

void
asm_emit_instruction_PLP(struct util_buffer* p_buf) {
  t2_bl(p_buf, g_t2.p_stub_plp);
}

void
asm_emit_instruction_TAX(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_X, REG_A);
}

void
asm_emit_instruction_TAY(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_Y, REG_A);
}

void
asm_emit_instruction_TSX(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_X, REG_S);
}

void
asm_emit_instruction_TXA(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_A, REG_X);
}

void
asm_emit_instruction_TXS(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_S, REG_X);
}

void
asm_emit_instruction_TYA(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_A, REG_Y);
}

/* Host N, Z from the low 8 bits of a register (C, V trashed, as the optimizer
 * expects of NZ flag uops).
 */
void
t2_emit_nz_flags(struct util_buffer* p_buf, uint32_t reg) {
  t2_shift(p_buf, k_t2_lsl, 1, REG_JIT_SCRATCH, reg, 24);
}

void
asm_emit_instruction_A_NZ_flags(struct util_buffer* p_buf) {
  t2_emit_nz_flags(p_buf, REG_A);
}

void
asm_emit_instruction_X_NZ_flags(struct util_buffer* p_buf) {
  t2_emit_nz_flags(p_buf, REG_X);
}

void
asm_emit_instruction_Y_NZ_flags(struct util_buffer* p_buf) {
  t2_emit_nz_flags(p_buf, REG_Y);
}

void
asm_emit_push_word_from_scratch(struct util_buffer* p_buf) {
  (void) p_buf;
  assert(0);
}

void
asm_emit_pull_word_to_scratch(struct util_buffer* p_buf) {
  (void) p_buf;
  assert(0);
}

/* Debugging through the JIT isn't supported on this backend. The symbol is
 * still needed: jit.c puts its address in the ABI struct.
 */
void
asm_debug_trampoline(void) {
  assert(0);
}
