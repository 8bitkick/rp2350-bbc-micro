#ifndef BEEBJIT_ASM_THUMB2_EMIT_H
#define BEEBJIT_ASM_THUMB2_EMIT_H

/* A small Thumb-2 (ARMv8-M Mainline, e.g. Cortex-M33) instruction encoder for
 * the thumb2 JIT backend. Instructions are written as halfwords into a
 * util_buffer, the first halfword of a 32-bit instruction first.
 *
 * Data processing uses the 32-bit encodings so that whether the condition
 * flags are written is always explicit (the 16-bit forms set them outside an
 * IT block).
 */

#include "../../util.h"

#include <assert.h>
#include <stdint.h>

enum {
  k_t2_r0 = 0, k_t2_r1, k_t2_r2, k_t2_r3, k_t2_r4, k_t2_r5, k_t2_r6, k_t2_r7,
  k_t2_r8, k_t2_r9, k_t2_r10, k_t2_r11, k_t2_r12, k_t2_sp, k_t2_lr, k_t2_pc,
};

enum {
  k_t2_eq = 0, k_t2_ne, k_t2_cs, k_t2_cc, k_t2_mi, k_t2_pl, k_t2_vs, k_t2_vc,
  k_t2_hi, k_t2_ls, k_t2_ge, k_t2_lt, k_t2_gt, k_t2_le, k_t2_al,
};

/* Data processing opcodes (the 4-bit "op" field). */
enum {
  k_t2_op_and = 0x0, k_t2_op_bic = 0x1, k_t2_op_orr = 0x2, k_t2_op_orn = 0x3,
  k_t2_op_eor = 0x4, k_t2_op_add = 0x8, k_t2_op_adc = 0xA, k_t2_op_sbc = 0xB,
  k_t2_op_sub = 0xD, k_t2_op_rsb = 0xE,
};

enum { k_t2_lsl = 0, k_t2_lsr = 1, k_t2_asr = 2, k_t2_ror = 3 };

static inline void
t2_h(struct util_buffer* p_buf, uint16_t h) {
  util_buffer_add_2b(p_buf, (h & 0xFF), (h >> 8));
}

static inline void
t2_w(struct util_buffer* p_buf, uint16_t h1, uint16_t h2) {
  t2_h(p_buf, h1);
  t2_h(p_buf, h2);
}

/* The 12-bit "modified immediate" for a value, or -1 if it has none. */
static inline int32_t
t2_mod_imm(uint32_t v) {
  uint32_t rot;
  if (v <= 0xFF) {
    return v;
  }
  if (((v & 0xFF00FF00) == 0) && ((v >> 16) == (v & 0xFF))) {
    return (0x100 | (v & 0xFF));
  }
  if (((v & 0x00FF00FF) == 0) && ((v >> 16) == (v & 0xFF00))) {
    return (0x200 | ((v >> 8) & 0xFF));
  }
  if (((v >> 24) == ((v >> 16) & 0xFF)) &&
      ((v >> 24) == ((v >> 8) & 0xFF)) &&
      ((v >> 24) == (v & 0xFF))) {
    return (0x300 | (v & 0xFF));
  }
  /* An 8-bit value with its top bit set, rotated right by 8..31. */
  for (rot = 8; rot < 32; ++rot) {
    uint32_t unrot = ((v << rot) | (v >> (32 - rot)));
    if ((unrot >= 0x80) && (unrot <= 0xFF)) {
      return (int32_t) ((rot << 7) | (unrot & 0x7F));
    }
  }
  return -1;
}

/* op{s} rd, rn, #imm (modified immediate); asserts the value is encodable. */
static inline void
t2_dp_imm(struct util_buffer* p_buf,
          uint32_t op,
          int s,
          uint32_t rd,
          uint32_t rn,
          uint32_t imm) {
  int32_t enc = t2_mod_imm(imm);
  assert(enc >= 0);
  t2_w(p_buf,
       (0xF000 | ((enc >> 11) << 10) | (op << 5) | (s << 4) | rn),
       ((((enc >> 8) & 7) << 12) | (rd << 8) | (enc & 0xFF)));
}

/* op{s} rd, rn, rm{, shift #n} */
static inline void
t2_dp_reg(struct util_buffer* p_buf,
          uint32_t op,
          int s,
          uint32_t rd,
          uint32_t rn,
          uint32_t rm,
          uint32_t shift_type,
          uint32_t shift_n) {
  assert(shift_n < 32);
  t2_w(p_buf,
       (0xEA00 | (op << 5) | (s << 4) | rn),
       (((shift_n >> 2) << 12) | (rd << 8) | ((shift_n & 3) << 6) |
        (shift_type << 4) | rm));
}

/* addw / subw rd, rn, #0..4095: never sets flags. */
static inline void
t2_addw(struct util_buffer* p_buf, uint32_t rd, uint32_t rn, uint32_t imm) {
  assert(imm <= 4095);
  t2_w(p_buf,
       (0xF200 | ((imm >> 11) << 10) | rn),
       ((((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 0xFF)));
}

static inline void
t2_subw(struct util_buffer* p_buf, uint32_t rd, uint32_t rn, uint32_t imm) {
  assert(imm <= 4095);
  t2_w(p_buf,
       (0xF2A0 | ((imm >> 11) << 10) | rn),
       ((((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 0xFF)));
}

/* movw / movt rd, #imm16 */
static inline void
t2_movw(struct util_buffer* p_buf, uint32_t rd, uint32_t imm) {
  assert(imm <= 0xFFFF);
  t2_w(p_buf,
       (0xF240 | (((imm >> 11) & 1) << 10) | (imm >> 12)),
       ((((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 0xFF)));
}

static inline void
t2_movt(struct util_buffer* p_buf, uint32_t rd, uint32_t imm) {
  assert(imm <= 0xFFFF);
  t2_w(p_buf,
       (0xF2C0 | (((imm >> 11) & 1) << 10) | (imm >> 12)),
       ((((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 0xFF)));
}

/* rd = any 32-bit constant, flags untouched. */
static inline void
t2_mov32(struct util_buffer* p_buf, uint32_t rd, uint32_t v) {
  if (t2_mod_imm(v) >= 0) {
    t2_dp_imm(p_buf, k_t2_op_orr, 0, rd, k_t2_pc, v);
    return;
  }
  t2_movw(p_buf, rd, (v & 0xFFFF));
  if (v >> 16) {
    t2_movt(p_buf, rd, (v >> 16));
  }
}

/* mov rd, rm (no flags). */
static inline void
t2_mov(struct util_buffer* p_buf, uint32_t rd, uint32_t rm) {
  t2_dp_reg(p_buf, k_t2_op_orr, 0, rd, k_t2_pc, rm, k_t2_lsl, 0);
}

/* {lsl,lsr,asr}{s} rd, rm, #n (as mov with a shift); n is 1..31. */
static inline void
t2_shift(struct util_buffer* p_buf,
         uint32_t type,
         int s,
         uint32_t rd,
         uint32_t rm,
         uint32_t n) {
  t2_dp_reg(p_buf, k_t2_op_orr, s, rd, k_t2_pc, rm, type, n);
}

/* ubfx rd, rn, #lsb, #width */
static inline void
t2_ubfx(struct util_buffer* p_buf,
        uint32_t rd,
        uint32_t rn,
        uint32_t lsb,
        uint32_t width) {
  t2_w(p_buf,
       (0xF3C0 | rn),
       (((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3) << 6) | (width - 1)));
}

/* bfi rd, rn, #lsb, #width */
static inline void
t2_bfi(struct util_buffer* p_buf,
       uint32_t rd,
       uint32_t rn,
       uint32_t lsb,
       uint32_t width) {
  uint32_t msb = (lsb + width - 1);
  t2_w(p_buf,
       (0xF360 | rn),
       (((lsb >> 2) << 12) | (rd << 8) | ((lsb & 3) << 6) | msb));
}

/* Loads and stores. size: 0 byte, 1 halfword, 2 word. */
static inline void
t2_ldst_imm(struct util_buffer* p_buf,
            int is_load,
            uint32_t size,
            uint32_t rt,
            uint32_t rn,
            uint32_t imm) {
  assert(imm <= 4095);
  t2_w(p_buf,
       (0xF880 | (size << 5) | (is_load << 4) | rn),
       ((rt << 12) | imm));
}

static inline void
t2_ldst_reg(struct util_buffer* p_buf,
            int is_load,
            uint32_t size,
            uint32_t rt,
            uint32_t rn,
            uint32_t rm,
            uint32_t lsl) {
  assert(lsl <= 3);
  t2_w(p_buf,
       (0xF800 | (size << 5) | (is_load << 4) | rn),
       ((rt << 12) | (lsl << 4) | rm));
}

static inline void t2_ldrb(struct util_buffer* p_buf,
                           uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 1, 0, rt, rn, imm);
}
static inline void t2_strb(struct util_buffer* p_buf,
                           uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 0, 0, rt, rn, imm);
}
static inline void t2_ldr(struct util_buffer* p_buf,
                          uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 1, 2, rt, rn, imm);
}
static inline void t2_str(struct util_buffer* p_buf,
                          uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 0, 2, rt, rn, imm);
}
static inline void t2_strh(struct util_buffer* p_buf,
                           uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 0, 1, rt, rn, imm);
}
static inline void t2_ldrh(struct util_buffer* p_buf,
                           uint32_t rt, uint32_t rn, uint32_t imm) {
  t2_ldst_imm(p_buf, 1, 1, rt, rn, imm);
}

/* ldrsb rt, [rn, #imm] (imm 0..4095). */
static inline void
t2_ldrsb(struct util_buffer* p_buf, uint32_t rt, uint32_t rn, uint32_t imm) {
  assert(imm <= 4095);
  t2_w(p_buf, (0xF990 | rn), ((rt << 12) | imm));
}

/* ldrb / strb / ldrh rt, [rn, #-imm] (imm 0..255). */
static inline void
t2_ldst_neg(struct util_buffer* p_buf,
            int is_load,
            uint32_t size,
            uint32_t rt,
            uint32_t rn,
            uint32_t imm) {
  assert(imm <= 255);
  t2_w(p_buf,
       (0xF800 | (size << 5) | (is_load << 4) | rn),
       ((rt << 12) | 0xC00 | imm));
}

static inline void t2_ldrb_reg(struct util_buffer* p_buf,
                               uint32_t rt, uint32_t rn, uint32_t rm) {
  t2_ldst_reg(p_buf, 1, 0, rt, rn, rm, 0);
}
static inline void t2_strb_reg(struct util_buffer* p_buf,
                               uint32_t rt, uint32_t rn, uint32_t rm) {
  t2_ldst_reg(p_buf, 0, 0, rt, rn, rm, 0);
}

/* mrs rd, apsr / msr apsr_nzcvq, rn */
static inline void
t2_mrs_apsr(struct util_buffer* p_buf, uint32_t rd) {
  t2_w(p_buf, 0xF3EF, (0x8000 | (rd << 8)));
}

static inline void
t2_msr_apsr(struct util_buffer* p_buf, uint32_t rn) {
  t2_w(p_buf, (0xF380 | rn), 0x8800);
}

/* it{...} cond: mask as in the encoding (0x8 for a single instruction). */
static inline void
t2_it(struct util_buffer* p_buf, uint32_t cond, uint32_t mask) {
  t2_h(p_buf, (0xBF00 | (cond << 4) | mask));
}

/* 16-bit mov rd, #imm8; only inside an IT block (where it sets no flags). */
static inline void
t2_it_movs8(struct util_buffer* p_buf, uint32_t rd, uint32_t imm) {
  assert(rd < 8);
  assert(imm <= 0xFF);
  t2_h(p_buf, (0x2000 | (rd << 8) | imm));
}

/* 16-bit mov rd, rm (any registers but pc): never sets flags, so it can
 * follow an IT.
 */
static inline void
t2_mov16(struct util_buffer* p_buf, uint32_t rd, uint32_t rm) {
  assert((rd != k_t2_pc) && (rm != k_t2_pc));
  t2_h(p_buf, (0x4600 | ((rd >> 3) << 7) | (rm << 3) | (rd & 7)));
}

/* 16-bit b<cond> over the next n bytes (n even, 2..256). */
static inline void
t2_bcond_skip(struct util_buffer* p_buf, uint32_t cond, uint32_t n) {
  assert((n & 1) == 0);
  assert((n >= 2) && (n <= 256));
  t2_h(p_buf, (0xD000 | (cond << 8) | ((n - 2) >> 1)));
}

static inline void
t2_bx(struct util_buffer* p_buf, uint32_t rm) {
  t2_h(p_buf, (0x4700 | (rm << 3)));
}

static inline void
t2_blx(struct util_buffer* p_buf, uint32_t rm) {
  t2_h(p_buf, (0x4780 | (rm << 3)));
}

static inline void
t2_nop(struct util_buffer* p_buf) {
  t2_h(p_buf, 0xBF00);
}

static inline void
t2_udf(struct util_buffer* p_buf, uint32_t imm8) {
  t2_h(p_buf, (0xDE00 | imm8));
}

/* Branch offsets are from the instruction's address + 4. */
static inline int32_t
t2_branch_offset(struct util_buffer* p_buf, void* p_target) {
  uintptr_t here = ((uintptr_t) util_buffer_get_base_address(p_buf) +
                    util_buffer_get_pos(p_buf));
  return (int32_t) ((uintptr_t) p_target - (here + 4));
}

/* b.w / bl, +-16MB. */
static inline void
t2_b_bl(struct util_buffer* p_buf, void* p_target, int is_link) {
  int32_t off = t2_branch_offset(p_buf, p_target);
  uint32_t s;
  uint32_t i1;
  uint32_t i2;
  uint32_t j1;
  uint32_t j2;
  assert((off & 1) == 0);
  assert((off >= -(1 << 24)) && (off < (1 << 24)));
  s = ((off >> 24) & 1);
  i1 = ((off >> 23) & 1);
  i2 = ((off >> 22) & 1);
  j1 = (!i1 ^ s);
  j2 = (!i2 ^ s);
  t2_w(p_buf,
       (0xF000 | (s << 10) | ((off >> 12) & 0x3FF)),
       ((is_link ? 0xD000 : 0x9000) | (j1 << 13) | (j2 << 11) |
        ((off >> 1) & 0x7FF)));
}

static inline void
t2_b(struct util_buffer* p_buf, void* p_target) {
  t2_b_bl(p_buf, p_target, 0);
}

static inline void
t2_bl(struct util_buffer* p_buf, void* p_target) {
  t2_b_bl(p_buf, p_target, 1);
}

static inline int
t2_bcond_in_range(struct util_buffer* p_buf, void* p_target) {
  int32_t off = t2_branch_offset(p_buf, p_target);
  return ((off >= -(1 << 20)) && (off < (1 << 20)));
}

/* b<cond>.w, +-1MB. */
static inline void
t2_bcond(struct util_buffer* p_buf, uint32_t cond, void* p_target) {
  int32_t off = t2_branch_offset(p_buf, p_target);
  uint32_t s;
  assert((off & 1) == 0);
  assert((off >= -(1 << 20)) && (off < (1 << 20)));
  s = ((off >> 20) & 1);
  t2_w(p_buf,
       (0xF000 | (s << 10) | (cond << 6) | ((off >> 12) & 0x3F)),
       (0x8000 | (((off >> 18) & 1) << 13) | (((off >> 19) & 1) << 11) |
        ((off >> 1) & 0x7FF)));
}

/* b<cond> anywhere in b.w range: direct if it reaches, otherwise the inverse
 * condition skips over a b.w. Always 6 bytes (the direct form is padded with
 * a nop): the JIT compiler sizes code, then may emit it again elsewhere.
 */
static inline void
t2_bcond_far(struct util_buffer* p_buf, uint32_t cond, void* p_target) {
  if (t2_bcond_in_range(p_buf, p_target)) {
    t2_bcond(p_buf, cond, p_target);
    t2_nop(p_buf);
    return;
  }
  /* 16-bit b<!cond> over the 4-byte b.w: 2 bytes on from pc (this + 4). */
  t2_h(p_buf, (0xD000 | ((cond ^ 1) << 8) | 0x01));
  t2_b(p_buf, p_target);
}

/* cbz / cbnz rn over the next n bytes (n even, 2..128): the offset is from
 * this instruction + 4, so it encodes n - 2.
 */
static inline void
t2_cbz_skip(struct util_buffer* p_buf, int is_nz, uint32_t rn, uint32_t n) {
  uint32_t off = (n - 2);
  assert(rn < 8);
  assert((n & 1) == 0);
  assert((n >= 2) && (n <= 128));
  t2_h(p_buf,
       ((is_nz ? 0xB900 : 0xB100) | (((off >> 6) & 1) << 9) |
        (((off >> 1) & 0x1F) << 3) | rn));
}

#endif /* BEEBJIT_ASM_THUMB2_EMIT_H */
