/* Thumb-2 (Cortex-M33) JIT backend for beebjit.
 *
 * Modelled on the ARM64 backend: the same uops and the same 6502 state model
 * (A, X, Y, S in registers; N, Z in the host flags; C, V in registers), with
 * the code emitted directly by a small encoder (thumb2_emit.h) rather than
 * copied from assembler templates. Register use is in thumb2_defs.h.
 *
 * Differences forced by a microcontroller (no MMU):
 * - JIT code lives at the fixed K_JIT_ADDR, which the host maps (PSRAM on the
 *   RP2350), 64 bytes per 6502 byte.
 * - Writes to JIT code don't fault: the 6502 store's write invalidation writes
 *   the "blx lr" invalidation marker directly, and lr holds the compile
 *   trampoline throughout JIT code.
 * - Memory is one flat 64k array (BEEBJIT_NO_MMU). Instructions whose address
 *   isn't known at compile time check it at run time (addr_check): stores at
 *   $8000 and up, and loads at $FC00 and up, go to the interpreter.
 * - The enter / exit / interpreter / compile trampolines are generated at the
 *   first entry into a stub page just after the JIT region, using offsetof()
 *   for every structure field (the shared asm_defs_host.h constants assume
 *   64-bit pointers). Each stub has a fixed address (thumb2_defs.h).
 * - Inturbo (asm_inturbo_thumb2.c) runs single instructions for the JIT:
 *   opcodes that keep being self-modified, and entries into the middle of an
 *   instruction. JIT code jumps to it with the PC in REG_INTURBO_PC.
 *
 * Part of the RP2350 port of beebjit; GPL v3, as beebjit.
 */
#include "../asm_jit.h"

#include "../../cpu_driver.h"
#include "../../defs_6502.h"
#include "../../log.h"
#include "../../memory_access.h"
#include "../../os_alloc.h"
#include "../../state_6502.h"
#include "../../util.h"
#include "../asm_common.h"
#include "../asm_inturbo_defs.h"
#include "../asm_jit_defs.h"
#include "../asm_opcodes.h"
#include "../asm_util.h"
#include "thumb2_defs.h"
#include "thumb2_emit.h"

#include "../../bbc.h"

#include <assert.h>
#include <stddef.h>

/* From asm_common_thumb2.c. */
void t2_emit_push(struct util_buffer* p_buf, uint32_t reg);
void t2_emit_pull(struct util_buffer* p_buf, uint32_t reg);
void t2_emit_flags_to_scratch1(struct util_buffer* p_buf);
void t2_emit_flags_from_scratch1(struct util_buffer* p_buf);
void t2_emit_nz_flags(struct util_buffer* p_buf, uint32_t reg);
void t2_emit_php_body(struct util_buffer* p_buf);
void t2_emit_plp_body(struct util_buffer* p_buf);

/* The head of jit.c's struct jit_struct: the fields JIT code references. */
struct t2_jit_head {
  struct cpu_driver driver;
  void* p_compile_callback;
  void* p_inturbo;
  uint32_t jit_ptrs[1];
};

struct asm_jit_struct {
  void* p_start;
  uint32_t length;
  struct os_alloc_mapping* p_stub_mapping;
};

static void
t2_flush(void* p, size_t length) {
  if (g_t2.p_flush_code != NULL) {
    g_t2.p_flush_code(p, length);
  }
}

int
asm_jit_is_enabled(void) {
  return 1;
}

int
asm_jit_is_default(void) {
  return 0;
}

void
asm_jit_test_preconditions(void) {
}

int
asm_jit_supports_uopcode(int32_t uopcode) {
  switch (uopcode) {
  case k_opcode_ST_IMM:
  case k_opcode_collapse_loop:
  case k_opcode_dex_loop_calc_countdown:
    return 0;
  default:
    return 1;
  }
}

int
asm_jit_uses_indirect_mappings(void) {
  return 0;
}

void
t2_layout_stubs(void) {
  uint8_t* p_stubs = (uint8_t*) K_T2_STUB_ADDR;
  g_t2.p_stub_compile = (p_stubs + K_T2_STUB_COMPILE);
  g_t2.p_stub_php = (p_stubs + K_T2_STUB_PHP);
  g_t2.p_stub_plp = (p_stubs + K_T2_STUB_PLP);
  g_t2.p_stub_pull16 = (p_stubs + K_T2_STUB_PULL16);
  g_t2.p_stub_exit = (p_stubs + K_T2_STUB_EXIT);
  g_t2.p_stub_interp = (p_stubs + K_T2_STUB_INTERP);
  g_t2.p_stub_flush_inv = (p_stubs + K_T2_STUB_FLUSH_INV);
  g_t2.p_stub_interp_inline = (p_stubs + K_T2_STUB_INTERP_INLINE);
  g_t2.p_stub_inturbo = (p_stubs + K_T2_STUB_INTURBO);
  g_t2.p_stub_inturbo_exit = (p_stubs + K_T2_STUB_INTURBO_EXIT);
  g_t2.p_stub_enter = (p_stubs + K_T2_STUB_ENTER);
  g_t2.p_stub_inturbo_interp = (p_stubs + K_T2_STUB_INTURBO_INTERP);
  g_t2.p_stub_write_flags = (p_stubs + K_T2_STUB_WRITE_FLAGS);
  g_t2.p_stub_inturbo_write_flags =
      (p_stubs + K_T2_STUB_INTURBO_WRITE_FLAGS);
}

struct asm_jit_struct*
asm_jit_create(void* p_jit_base,
               int (*is_memory_always_ram)(void* p, uint16_t addr),
               void* p_memory_object) {
  struct asm_jit_struct* p_asm = util_mallocz(sizeof(struct asm_jit_struct));
  (void) is_memory_always_ram;
  (void) p_memory_object;

  /* The emitted code computes host addresses from K_JIT_ADDR, as an
   * immediate.
   */
  if (t2_mod_imm(K_JIT_ADDR) < 0) {
    util_bail("thumb2 JIT: K_JIT_ADDR isn't a Thumb-2 immediate");
  }
  if (p_jit_base != (void*) K_JIT_ADDR) {
    util_bail("thumb2 JIT: code region not at K_JIT_ADDR");
  }
  t2_layout_stubs();
  p_asm->p_stub_mapping = os_alloc_get_mapping((void*) K_T2_STUB_ADDR,
                                               K_T2_STUB_SIZE);
  if (os_alloc_get_mapping_addr(p_asm->p_stub_mapping) !=
      (void*) K_T2_STUB_ADDR) {
    util_bail("thumb2 JIT: stub page not at its fixed address");
  }
  return p_asm;
}

void
asm_jit_destroy(struct asm_jit_struct* p_asm) {
  os_alloc_free_mapping(p_asm->p_stub_mapping);
  util_free(p_asm);
}

void*
asm_jit_get_private(struct asm_jit_struct* p_asm) {
  (void) p_asm;
  return NULL;
}

void
asm_jit_start_code_updates(struct asm_jit_struct* p_asm,
                           void* p_start,
                           uint32_t length) {
  if (p_start == NULL) {
    p_start = (void*) K_JIT_ADDR;
    length = K_JIT_SIZE;
  }
  p_asm->p_start = p_start;
  p_asm->length = length;
}

void
asm_jit_finish_code_updates(struct asm_jit_struct* p_asm) {
  t2_flush(p_asm->p_start, p_asm->length);
}

int
asm_jit_handle_fault(struct asm_jit_struct* p_asm,
                     uintptr_t* p_pc,
                     int is_inturbo,
                     int32_t addr_6502,
                     void* p_fault_addr,
                     int is_write) {
  (void) p_asm;
  (void) p_pc;
  (void) is_inturbo;
  (void) addr_6502;
  (void) p_fault_addr;
  (void) is_write;
  /* No faults are expected: there's no MMU. */
  return 0;
}

void
asm_jit_invalidate_code_at(void* p) {
  *(volatile uint16_t*) p = K_T2_INVALIDATED;
}

int
asm_jit_is_invalidated_code_at(void* p) {
  return (*(volatile uint16_t*) p == K_T2_INVALIDATED);
}

void
asm_emit_jit_invalidated(struct util_buffer* p_buf) {
  t2_h(p_buf, K_T2_INVALIDATED);
}

/* Backend-specific uopcodes set by asm_jit_rewrite() in backend_tag. */
enum {
  k_opcode_t2_addr_load_byte = 0x1000,
  k_opcode_t2_addr_trunc_8bit,
  k_opcode_t2_check_page_crossing_ABX,
  k_opcode_t2_check_page_crossing_ABY,
  k_opcode_t2_load_byte_pair,
  k_opcode_t2_load_byte_pair_base,
  k_opcode_t2_mode_ABX,
  k_opcode_t2_mode_ABY,
  k_opcode_t2_value_load_ABS,
  k_opcode_t2_value_store_ABS,
  k_opcode_t2_write_inv_ABS,
  k_opcode_t2_ADC_IMM,
  k_opcode_t2_ADD_IMM,
  k_opcode_t2_AND_IMM,
  k_opcode_t2_CMP_IMM,
  k_opcode_t2_CPX_IMM,
  k_opcode_t2_CPY_IMM,
  k_opcode_t2_EOR_IMM,
  k_opcode_t2_LDA_ABS,
  k_opcode_t2_LDA_addr,
  k_opcode_t2_LDA_IMM,
  k_opcode_t2_LDX_ABS,
  k_opcode_t2_LDX_addr,
  k_opcode_t2_LDX_IMM,
  k_opcode_t2_LDY_ABS,
  k_opcode_t2_LDY_addr,
  k_opcode_t2_LDY_IMM,
  k_opcode_t2_ORA_IMM,
  k_opcode_t2_SBC_IMM,
  k_opcode_t2_STA_ABS,
  k_opcode_t2_STA_addr,
  k_opcode_t2_STX_ABS,
  k_opcode_t2_STX_addr,
  k_opcode_t2_STY_ABS,
  k_opcode_t2_STY_addr,
  k_opcode_t2_SUB_IMM,
};

/* Specialise the generic uops of one 6502 opcode, as the ARM64 backend does:
 * fold known addresses into loads and stores (ldrb / strb take a 12-bit
 * offset, as ARM64's do), immediates into the operations, and loads and
 * stores into LDA / STA etc. This is about density as much as speed: an
 * opcode's code must fit in its 64 bytes per 6502 byte.
 */
void
asm_jit_rewrite(struct asm_jit_struct* p_asm,
                struct asm_uop* p_uops,
                uint32_t num_uops) {
  struct asm_uop* p_main_uop;
  struct asm_uop* p_mode_uop;
  struct asm_uop* p_load_uop;
  struct asm_uop* p_store_uop;
  struct asm_uop* p_load_carry_uop;
  struct asm_uop* p_save_carry_uop;
  struct asm_uop* p_load_overflow_uop;
  struct asm_uop* p_save_overflow_uop;
  struct asm_uop* p_flags_uop;
  struct asm_uop* p_inv_uop;
  struct asm_uop* p_addr_check_uop;
  struct asm_uop* p_page_crossing_uop;
  struct asm_uop* p_tmp_uop;
  int32_t uopcode;
  intptr_t value1;
  int is_imm;
  int is_abs;
  uint16_t addr;
  (void) p_asm;

  asm_breakdown_from_6502(p_uops,
                          num_uops,
                          &p_main_uop,
                          &p_mode_uop,
                          &p_load_uop,
                          &p_store_uop,
                          &p_load_carry_uop,
                          &p_save_carry_uop,
                          &p_load_overflow_uop,
                          &p_save_overflow_uop,
                          &p_flags_uop,
                          &p_inv_uop,
                          &p_addr_check_uop,
                          &p_page_crossing_uop);
  if (p_main_uop == NULL) {
    return;
  }

  /* As ARM64: every operation that takes the carry reads REG_CF itself, so
   * the carry is never left only in the host flag between instructions.
   */
  if ((p_load_carry_uop != NULL) && !p_load_carry_uop->is_eliminated) {
    p_load_carry_uop->is_eliminated = 1;
    p_load_carry_uop->is_merged = 1;
  }

  /* Run time address checks: value2 says whether the operation writes. */
  if (p_addr_check_uop != NULL) {
    p_addr_check_uop->value2 = (p_store_uop != NULL);
  }

  /* ROL A / ROR A (possibly several merged) set REG_CF directly. */
  switch (p_main_uop->uopcode) {
  case k_opcode_ROL_acc:
  case k_opcode_ROR_acc:
    assert(p_save_carry_uop != NULL);
    p_save_carry_uop->is_eliminated = 1;
    p_save_carry_uop->is_merged = 1;
    break;
  default:
    break;
  }

  /* ADD / SUB / compares set the host N and Z themselves. */
  switch (p_main_uop->uopcode) {
  case k_opcode_ADD:
  case k_opcode_CMP:
  case k_opcode_CPX:
  case k_opcode_CPY:
  case k_opcode_SUB:
    assert(p_flags_uop != NULL);
    p_flags_uop->is_eliminated = 1;
    p_flags_uop->is_merged = 1;
    break;
  default:
    break;
  }

  if (p_mode_uop == NULL) {
    return;
  }

  is_imm = 0;
  is_abs = 0;
  addr = 0;

  uopcode = p_mode_uop->uopcode;
  if ((uopcode == k_opcode_addr_add_x) || (uopcode == k_opcode_addr_add_y)) {
    p_tmp_uop = (p_mode_uop - 1);
    if (p_tmp_uop->uopcode == k_opcode_addr_load_16bit_nowrap) {
      /* Back up for dynamic operand load. */
      p_mode_uop = p_tmp_uop;
    }
  }

  /* By default, loads and stores use the address in REG_ADDR directly. */
  switch (p_main_uop->uopcode) {
  case k_opcode_LDA: p_main_uop->backend_tag = k_opcode_t2_LDA_addr; break;
  case k_opcode_LDX: p_main_uop->backend_tag = k_opcode_t2_LDX_addr; break;
  case k_opcode_LDY: p_main_uop->backend_tag = k_opcode_t2_LDY_addr; break;
  case k_opcode_STA: p_main_uop->backend_tag = k_opcode_t2_STA_addr; break;
  case k_opcode_STX: p_main_uop->backend_tag = k_opcode_t2_STX_addr; break;
  case k_opcode_STY: p_main_uop->backend_tag = k_opcode_t2_STY_addr; break;
  default: break;
  }

  uopcode = p_mode_uop->uopcode;
  value1 = p_mode_uop->value1;
  switch (uopcode) {
  case k_opcode_value_set:
    /* Mode IMM. */
    is_imm = 1;
    switch (p_main_uop->uopcode) {
    case k_opcode_ADC: case k_opcode_ADD: case k_opcode_AND: case k_opcode_CMP:
    case k_opcode_CPX: case k_opcode_CPY: case k_opcode_EOR: case k_opcode_LDA:
    case k_opcode_LDX: case k_opcode_LDY: case k_opcode_ORA: case k_opcode_SBC:
    case k_opcode_SUB:
      p_mode_uop->is_eliminated = 1;
      p_main_uop->value1 = value1;
      switch (p_main_uop->uopcode) {
      case k_opcode_ADC: p_main_uop->backend_tag = k_opcode_t2_ADC_IMM; break;
      case k_opcode_ADD: p_main_uop->backend_tag = k_opcode_t2_ADD_IMM; break;
      case k_opcode_AND: p_main_uop->backend_tag = k_opcode_t2_AND_IMM; break;
      case k_opcode_CMP: p_main_uop->backend_tag = k_opcode_t2_CMP_IMM; break;
      case k_opcode_CPX: p_main_uop->backend_tag = k_opcode_t2_CPX_IMM; break;
      case k_opcode_CPY: p_main_uop->backend_tag = k_opcode_t2_CPY_IMM; break;
      case k_opcode_EOR: p_main_uop->backend_tag = k_opcode_t2_EOR_IMM; break;
      case k_opcode_LDA: p_main_uop->backend_tag = k_opcode_t2_LDA_IMM; break;
      case k_opcode_LDX: p_main_uop->backend_tag = k_opcode_t2_LDX_IMM; break;
      case k_opcode_LDY: p_main_uop->backend_tag = k_opcode_t2_LDY_IMM; break;
      case k_opcode_ORA: p_main_uop->backend_tag = k_opcode_t2_ORA_IMM; break;
      case k_opcode_SBC: p_main_uop->backend_tag = k_opcode_t2_SBC_IMM; break;
      case k_opcode_SUB: p_main_uop->backend_tag = k_opcode_t2_SUB_IMM; break;
      default: assert(0); break;
      }
      break;
    default:
      /* e.g. ALR, NOP #imm: the generic uops. */
      break;
    }
    break;
  case k_opcode_addr_set:
    /* Mode ABS or ZPG. */
    is_abs = 1;
    addr = value1;
    if (addr >= 0x1000) {
      break;
    }
    switch (p_main_uop->uopcode) {
    case k_opcode_LDA:
    case k_opcode_LDX:
    case k_opcode_LDY:
    case k_opcode_STA:
    case k_opcode_STX:
    case k_opcode_STY:
      p_mode_uop->is_eliminated = 1;
      p_main_uop->value1 = addr;
      switch (p_main_uop->uopcode) {
      case k_opcode_LDA: p_main_uop->backend_tag = k_opcode_t2_LDA_ABS; break;
      case k_opcode_LDX: p_main_uop->backend_tag = k_opcode_t2_LDX_ABS; break;
      case k_opcode_LDY: p_main_uop->backend_tag = k_opcode_t2_LDY_ABS; break;
      case k_opcode_STA: p_main_uop->backend_tag = k_opcode_t2_STA_ABS; break;
      case k_opcode_STX: p_main_uop->backend_tag = k_opcode_t2_STX_ABS; break;
      case k_opcode_STY: p_main_uop->backend_tag = k_opcode_t2_STY_ABS; break;
      default: assert(0); break;
      }
      break;
    default:
      p_mode_uop->is_eliminated = 1;
      if (p_load_uop != NULL) {
        p_load_uop->backend_tag = k_opcode_t2_value_load_ABS;
        p_load_uop->value1 = addr;
      }
      if (p_store_uop != NULL) {
        p_store_uop->backend_tag = k_opcode_t2_value_store_ABS;
        p_store_uop->value1 = addr;
      }
      break;
    }
    break;
  case k_opcode_value_load_16bit_wrap:
    /* Mode IND (JMP): the pointer's address is known. */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    addr = p_mode_uop->value1;
    p_mode_uop->is_eliminated = 1;
    p_mode_uop++;
    p_mode_uop->backend_tag = k_opcode_t2_load_byte_pair;
    p_mode_uop->value1 = addr;
    /* Wraps around 0x10FF -> 0x1000. */
    addr = (addr + 1);
    if (!(addr & 0xFF)) addr -= 0x100;
    p_mode_uop->value2 = addr;
    break;
  case k_opcode_addr_load_16bit_nowrap:
    /* Fetch for a 16-bit dynamic operand. */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    addr = p_mode_uop->value1;
    p_mode_uop->is_eliminated = 1;
    p_mode_uop++;
    p_mode_uop->backend_tag = k_opcode_t2_load_byte_pair;
    p_mode_uop->value1 = addr;
    p_mode_uop->value2 = (uint16_t) (addr + 1);
    break;
  case k_opcode_addr_load_8bit:
    /* Fetch for an 8-bit dynamic operand (mode ZPG). */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    addr = p_mode_uop->value1;
    p_mode_uop->is_eliminated = 1;
    p_mode_uop++;
    p_mode_uop->backend_tag = k_opcode_t2_addr_load_byte;
    p_mode_uop->value1 = addr;
    break;
  case k_opcode_addr_add_x_8bit:
  case k_opcode_addr_add_y_8bit:
    /* Mode ZPX / ZPY: index + base, then truncate. */
    p_mode_uop->backend_tag = k_opcode_t2_addr_trunc_8bit;
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    p_mode_uop->backend_tag = ((uopcode == k_opcode_addr_add_x_8bit) ?
                               k_opcode_t2_mode_ABX : k_opcode_t2_mode_ABY);
    break;
  case k_opcode_addr_add_x:
  case k_opcode_addr_add_y:
    /* Mode ABX / ABY. */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    addr = p_mode_uop->value1;
    if (addr < 0x1000) {
      p_mode_uop->is_eliminated = 1;
      p_mode_uop++;
      p_mode_uop->backend_tag = ((uopcode == k_opcode_addr_add_x) ?
                                 k_opcode_t2_mode_ABX : k_opcode_t2_mode_ABY);
      p_mode_uop->value1 = addr;
    }
    if (p_page_crossing_uop != NULL) {
      p_page_crossing_uop->backend_tag =
          ((uopcode == k_opcode_addr_add_x) ?
           k_opcode_t2_check_page_crossing_ABX :
           k_opcode_t2_check_page_crossing_ABY);
      p_page_crossing_uop->value1 = addr;
    }
    break;
  case k_opcode_addr_load_16bit_wrap:
    /* Mode IDX. */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_add_x_8bit);
    p_mode_uop->backend_tag = k_opcode_t2_addr_trunc_8bit;
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    p_mode_uop->backend_tag = k_opcode_t2_mode_ABX;
    break;
  case k_opcode_addr_add_base_y:
  case k_opcode_addr_add_base_constant:
    /* Mode IDY, Y known or unknown: the zero page pointer address is known. */
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_base_load_16bit_wrap);
    p_mode_uop--;
    assert(p_mode_uop->uopcode == k_opcode_addr_set);
    addr = p_mode_uop->value1;
    p_mode_uop->is_eliminated = 1;
    p_mode_uop++;
    p_mode_uop->backend_tag = k_opcode_t2_load_byte_pair_base;
    p_mode_uop->value1 = addr;
    p_mode_uop->value2 = (uint8_t) (addr + 1);
    break;
  default:
    assert(0);
    break;
  }

  /* Loads and stores are built into the LDx / STx operations. */
  if (!is_imm) {
    switch (p_main_uop->uopcode) {
    case k_opcode_LDA:
    case k_opcode_LDX:
    case k_opcode_LDY:
      assert(p_load_uop != NULL);
      p_load_uop->is_eliminated = 1;
      break;
    case k_opcode_STA:
    case k_opcode_STX:
    case k_opcode_STY:
      assert(p_store_uop != NULL);
      p_store_uop->is_eliminated = 1;
      break;
    default:
      break;
    }
  }

  if ((p_inv_uop != NULL) && is_abs && (addr < 0x1000)) {
    p_inv_uop->backend_tag = k_opcode_t2_write_inv_ABS;
    p_inv_uop->value1 = addr;
  }
}

/* ---- Emission helpers ---- */

/* To the interpreter at addr_6502: bl to a stub that reads the address from
 * the halfword after the bl (6 bytes; movw + b would be 8).
 */
static void
t2_emit_jump_interp(struct util_buffer* p_buf, uint16_t addr_6502) {
  t2_bl(p_buf, g_t2.p_stub_interp_inline);
  t2_h(p_buf, addr_6502);
}

/* If REG_JIT_SCRATCH is non-zero, go to the interpreter at addr_6502. Inline
 * (cbz over the exit) rather than in an epilog: it's smaller. The branch
 * doesn't touch the flags.
 */
static void
t2_emit_exit_if_scratch(struct util_buffer* p_buf,
                        struct util_buffer* p_buf_epilog,
                        uint16_t addr_6502) {
  (void) p_buf_epilog;
  t2_cbz_skip(p_buf, 0, REG_JIT_SCRATCH, 6);
  t2_emit_jump_interp(p_buf, addr_6502);
}

static void
t2_emit_load_abs(struct util_buffer* p_buf, uint32_t reg, uint32_t addr) {
  if (addr <= 4095) {
    t2_ldrb(p_buf, reg, REG_MEM, addr);
  } else {
    t2_mov32(p_buf, reg, addr);
    t2_ldrb_reg(p_buf, reg, REG_MEM, reg);
  }
}

static void
t2_emit_add8(struct util_buffer* p_buf, uint32_t reg, int32_t delta) {
  if (delta >= 0) {
    t2_addw(p_buf, reg, reg, (uint32_t) delta);
  } else {
    t2_subw(p_buf, reg, reg, (uint32_t) -delta);
  }
  t2_dp_imm(p_buf, k_t2_op_and, 0, reg, reg, 0xFF);
}

/* reg = 16-bit little endian pointer at zero page (base, base + 1 wrapping in
 * the zero page), base in REG_ADDR.
 */
static void
t2_emit_load_zp_pointer(struct util_buffer* p_buf, uint32_t reg) {
  t2_addw(p_buf, REG_JIT_SCRATCH, REG_ADDR, 1);
  t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 0xFF);
  t2_ldrb_reg(p_buf, REG_JIT_SCRATCH, REG_MEM, REG_JIT_SCRATCH);
  t2_ldrb_reg(p_buf, reg, REG_MEM, REG_ADDR);
  t2_dp_reg(p_buf, k_t2_op_orr, 0, reg, reg, REG_JIT_SCRATCH, k_t2_lsl, 8);
}

/* Host C -> REG_CF without touching the flags. */
static void
t2_emit_save_carry(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_CF, 0);
  t2_dp_imm(p_buf, k_t2_op_adc, 0, REG_CF, REG_CF, 0);
}

/* reg = (reg << 1 | C) & 0xFF, host C = old bit 7; REG_CF is the carry in. */
static void
t2_emit_rol(struct util_buffer* p_buf, uint32_t reg) {
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, reg, 24);
  t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, REG_CF,
            k_t2_lsl, 23);
  t2_shift(p_buf, k_t2_lsl, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
  t2_shift(p_buf, k_t2_lsr, 0, reg, REG_JIT_SCRATCH, 24);
}

/* reg = (reg >> 1) | (C << 7), host C = old bit 0. */
static void
t2_emit_ror(struct util_buffer* p_buf, uint32_t reg) {
  t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, reg, REG_CF, k_t2_lsl, 8);
  t2_shift(p_buf, k_t2_lsr, 1, reg, REG_JIT_SCRATCH, 1);
}

/* 8-bit compare: host NZC from reg - REG_VALUE, the result into REG_VALUE. */
static void
t2_emit_compare(struct util_buffer* p_buf, uint32_t reg) {
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, reg, 24);
  t2_dp_reg(p_buf, k_t2_op_sub, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
            REG_VALUE, k_t2_lsl, 24);
  t2_shift(p_buf, k_t2_lsr, 0, REG_VALUE, REG_JIT_SCRATCH, 24);
}

/* Branch if REG_CF / REG_OF is (non-)zero, flags untouched. */
static void
t2_emit_branch_reg(struct util_buffer* p_buf,
                   uint32_t reg,
                   int if_set,
                   void* p_target) {
  t2_cbz_skip(p_buf, if_set ? 0 : 1, reg, 4);
  t2_b(p_buf, p_target);
}

/* The page crossing refund: countdown += 1 if (a ^ b), in REG_JIT_SCRATCH,
 * has bit 8 clear (the cycle counts assume a crossing).
 */
static void
t2_emit_page_cross_refund(struct util_buffer* p_buf) {
  /* mvn scratch, scratch, lsl #23: bit 31 = !bit 8. */
  t2_dp_reg(p_buf, k_t2_op_orn, 0, REG_JIT_SCRATCH, k_t2_pc, REG_JIT_SCRATCH,
            k_t2_lsl, 23);
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_COUNTDOWN, REG_COUNTDOWN,
            REG_JIT_SCRATCH, k_t2_lsr, 31);
}

/* reg = the byte at a known address. */
static void
t2_emit_load_byte_abs(struct util_buffer* p_buf, uint32_t reg, uint32_t addr) {
  if (addr <= 4095) {
    t2_ldrb(p_buf, reg, REG_MEM, addr);
  } else {
    t2_mov32(p_buf, reg, addr);
    t2_ldrb_reg(p_buf, reg, REG_MEM, reg);
  }
}

/* 8-bit ADC / ADD / SBC / SUB of an immediate into A. */
static void
t2_emit_arith_imm(struct util_buffer* p_buf, int32_t uopcode, uint32_t imm) {
  uint32_t op = 0;
  int use_carry = 0;
  switch (uopcode) {
  case k_opcode_t2_ADC_IMM: op = k_t2_op_adc; use_carry = 1; break;
  case k_opcode_t2_ADD_IMM: op = k_t2_op_add; break;
  case k_opcode_t2_SBC_IMM: op = k_t2_op_sbc; use_carry = 1; break;
  case k_opcode_t2_SUB_IMM: op = k_t2_op_sub; break;
  default: assert(0); break;
  }
  if (op == k_t2_op_adc) {
    /* A << 24 | 0xFFFFFF, so the carry in ripples into bit 24. */
    t2_dp_imm(p_buf, k_t2_op_orn, 0, REG_JIT_SCRATCH, k_t2_pc, 0xFF000000);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, REG_A,
              k_t2_lsl, 24);
  } else {
    t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_A, 24);
  }
  if (use_carry) {
    t2_shift(p_buf, k_t2_lsr, 1, REG_SCRATCH1, REG_CF, 1);
  }
  t2_dp_imm(p_buf, op, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH, (imm << 24));
  t2_shift(p_buf, k_t2_lsr, 0, REG_A, REG_JIT_SCRATCH, 24);
}

/* Compare a register with an immediate: host NZC, the result in REG_VALUE. */
static void
t2_emit_compare_imm(struct util_buffer* p_buf, uint32_t reg, uint32_t imm) {
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, reg, 24);
  t2_dp_imm(p_buf, k_t2_op_sub, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
            (imm << 24));
  t2_shift(p_buf, k_t2_lsr, 0, REG_VALUE, REG_JIT_SCRATCH, 24);
}

/* On a host whose instruction fetch doesn't see data writes (the a32 test
 * harness on a Cortex-A, which sets p_flush_code), a write invalidation must
 * also be flushed. Not needed on the RP2350 (the XIP cache is unified, and
 * the M33 has no instruction cache) or in QEMU.
 */
static void
t2_emit_flush_inv(struct util_buffer* p_buf) {
  if (g_t2.p_flush_code != NULL) {
    t2_bl(p_buf, g_t2.p_stub_flush_inv);
  }
}

/* After a store to a known address: the page's flags (bbc.h), and the
 * write-flags stub if there are any (a code page to invalidate in, or the
 * screen to mark changed).
 */
static void
t2_emit_write_inv_abs(struct util_buffer* p_buf, uint32_t addr) {
  uint32_t p_flag = (uint32_t) (uintptr_t) &g_bbc_page_flags[addr >> 8];
  t2_movw(p_buf, REG_SCRATCH1, (p_flag & 0xFFFF));
  t2_movt(p_buf, REG_SCRATCH1, (p_flag >> 16));
  t2_ldrb(p_buf, REG_JIT_SCRATCH, REG_SCRATCH1, 0);
  t2_cbz_skip(p_buf, 0, REG_JIT_SCRATCH, 8);
  /* The store itself used the address as an immediate (addr_set is
   * eliminated): REG_ADDR is free, and the stub wants the address in it.
   */
  t2_movw(p_buf, REG_ADDR, addr);
  t2_bl(p_buf, g_t2.p_stub_write_flags);
}

/* The write-flags stub (thumb2_defs.h): the address in reg_addr, the flags
 * in r7 (REG_JIT_SCRATCH / inturbo's T). Sets no host flags: the 6502's N
 * and Z live there across a store. For the JIT, lr leaves as the compile
 * stub; for inturbo (which saved its countdown from lr), lr returns.
 */
static void
t2_emit_write_flags_stub(struct util_buffer* p_buf,
                         uint32_t reg_addr,
                         int is_inturbo) {
  uint32_t p_dirty = (uint32_t) (uintptr_t) &g_bbc_page_flags[k_bbc_page_flags_dirty];
  uint32_t p_ptrs = (uint32_t) (uintptr_t) g_t2.p_jit_ptrs;
  /* r6 (REG_VALUE) is a scratch here: pushed. */
  uint32_t tmp = k_t2_r6;
  /* push {r6, r7, lr} */
  t2_h(p_buf, 0xB5C0);
  /* The screen bit: mark the picture changed (any non-zero byte). */
  t2_ubfx(p_buf, k_t2_r7, k_t2_r7, 1, 1);
  t2_cbz_skip(p_buf, 0, k_t2_r7, 12);
  t2_movw(p_buf, tmp, (p_dirty & 0xFFFF));
  t2_movt(p_buf, tmp, (p_dirty >> 16));
  t2_ldst_imm(p_buf, 0, 0, k_t2_r7, tmp, 0);
  /* The code bit: invalidate the JIT code for the address (an address with
   * no code points at the no-code page, which takes the write).
   */
  t2_ldst_imm(p_buf, 1, 2, k_t2_r7, k_t2_sp, 4);
  t2_ubfx(p_buf, k_t2_r7, k_t2_r7, 0, 1);
  t2_cbz_skip(p_buf, 0, k_t2_r7, (20 + ((g_t2.p_flush_code != NULL) ? 4 : 0)));
  t2_movw(p_buf, k_t2_r7, (p_ptrs & 0xFFFF));
  t2_movt(p_buf, k_t2_r7, (p_ptrs >> 16));
  t2_ldst_reg(p_buf, 1, 2, k_t2_r7, k_t2_r7, reg_addr, 2);
  t2_movw(p_buf, tmp, K_T2_INVALIDATED);
  t2_strh(p_buf, tmp, k_t2_r7, 0);
  t2_emit_flush_inv(p_buf);
  if (is_inturbo) {
    /* pop {r6, r7, pc} */
    t2_h(p_buf, 0xBDC0);
  } else {
    /* pop {r6, r7}; ldr lr, [sp], #4 -> r8; lr = the compile stub. */
    t2_h(p_buf, 0xBCC0);
    t2_w(p_buf, 0xF85D, 0x8B04);
    t2_mov32(p_buf, k_t2_lr, ((uint32_t) (uintptr_t) g_t2.p_stub_compile | 1));
    t2_bx(p_buf, REG_SCRATCH1);
  }
}

void
asm_emit_jit(struct asm_jit_struct* p_asm,
             struct util_buffer* p_buf,
             struct util_buffer* p_buf_epilog,
             struct asm_uop* p_uop) {
  int32_t uopcode = p_uop->uopcode;
  intptr_t value1 = p_uop->value1;
  intptr_t value2 = p_uop->value2;
  (void) p_asm;

  if (p_uop->backend_tag >= 0x1000) {
    uopcode = p_uop->backend_tag;
  }

  switch (uopcode) {
  case k_opcode_t2_addr_load_byte:
    t2_emit_load_byte_abs(p_buf, REG_ADDR, value1);
    break;
  case k_opcode_t2_addr_trunc_8bit:
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_ADDR, REG_ADDR, 0xFF);
    break;
  case k_opcode_t2_check_page_crossing_ABX:
  case k_opcode_t2_check_page_crossing_ABY:
    /* The base is known: no crossing (a cycle back) if index < 0x100 - lo. */
    t2_subw(p_buf, REG_JIT_SCRATCH,
            ((uopcode == k_opcode_t2_check_page_crossing_ABX) ? REG_X : REG_Y),
            (0x100 - (value1 & 0xFF)));
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_COUNTDOWN, REG_COUNTDOWN,
              REG_JIT_SCRATCH, k_t2_lsr, 31);
    break;
  case k_opcode_t2_load_byte_pair:
  case k_opcode_t2_load_byte_pair_base:
    {
      uint32_t reg = ((uopcode == k_opcode_t2_load_byte_pair) ?
                      REG_ADDR : REG_ADDR_BASE);
      if ((value2 == (value1 + 1)) && (value1 < 4095)) {
        /* No wrap: one (possibly unaligned) halfword load. */
        t2_ldrh(p_buf, reg, REG_MEM, value1);
      } else {
        t2_emit_load_byte_abs(p_buf, reg, value1);
        t2_emit_load_byte_abs(p_buf, REG_JIT_SCRATCH, value2);
        t2_dp_reg(p_buf, k_t2_op_orr, 0, reg, reg, REG_JIT_SCRATCH,
                  k_t2_lsl, 8);
      }
    }
    break;
  case k_opcode_t2_mode_ABX:
    t2_addw(p_buf, REG_ADDR, REG_X, value1);
    break;
  case k_opcode_t2_mode_ABY:
    t2_addw(p_buf, REG_ADDR, REG_Y, value1);
    break;
  case k_opcode_t2_value_load_ABS:
    t2_ldrb(p_buf, REG_VALUE, REG_MEM, value1);
    break;
  case k_opcode_t2_value_store_ABS:
    t2_strb(p_buf, REG_VALUE, REG_MEM, value1);
    break;
  case k_opcode_t2_write_inv_ABS:
    t2_emit_write_inv_abs(p_buf, value1);
    break;
  case k_opcode_t2_ADC_IMM:
  case k_opcode_t2_ADD_IMM:
  case k_opcode_t2_SBC_IMM:
  case k_opcode_t2_SUB_IMM:
    t2_emit_arith_imm(p_buf, uopcode, value1);
    break;
  case k_opcode_t2_AND_IMM:
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_A, REG_A, value1);
    break;
  case k_opcode_t2_EOR_IMM:
    t2_dp_imm(p_buf, k_t2_op_eor, 0, REG_A, REG_A, value1);
    break;
  case k_opcode_t2_ORA_IMM:
    t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_A, REG_A, value1);
    break;
  case k_opcode_t2_CMP_IMM: t2_emit_compare_imm(p_buf, REG_A, value1); break;
  case k_opcode_t2_CPX_IMM: t2_emit_compare_imm(p_buf, REG_X, value1); break;
  case k_opcode_t2_CPY_IMM: t2_emit_compare_imm(p_buf, REG_Y, value1); break;
  case k_opcode_t2_LDA_ABS: t2_ldrb(p_buf, REG_A, REG_MEM, value1); break;
  case k_opcode_t2_LDX_ABS: t2_ldrb(p_buf, REG_X, REG_MEM, value1); break;
  case k_opcode_t2_LDY_ABS: t2_ldrb(p_buf, REG_Y, REG_MEM, value1); break;
  case k_opcode_t2_LDA_addr: t2_ldrb_reg(p_buf, REG_A, REG_MEM, REG_ADDR); break;
  case k_opcode_t2_LDX_addr: t2_ldrb_reg(p_buf, REG_X, REG_MEM, REG_ADDR); break;
  case k_opcode_t2_LDY_addr: t2_ldrb_reg(p_buf, REG_Y, REG_MEM, REG_ADDR); break;
  case k_opcode_t2_LDA_IMM: t2_mov32(p_buf, REG_A, value1); break;
  case k_opcode_t2_LDX_IMM: t2_mov32(p_buf, REG_X, value1); break;
  case k_opcode_t2_LDY_IMM: t2_mov32(p_buf, REG_Y, value1); break;
  case k_opcode_t2_STA_ABS: t2_strb(p_buf, REG_A, REG_MEM, value1); break;
  case k_opcode_t2_STX_ABS: t2_strb(p_buf, REG_X, REG_MEM, value1); break;
  case k_opcode_t2_STY_ABS: t2_strb(p_buf, REG_Y, REG_MEM, value1); break;
  case k_opcode_t2_STA_addr: t2_strb_reg(p_buf, REG_A, REG_MEM, REG_ADDR); break;
  case k_opcode_t2_STX_addr: t2_strb_reg(p_buf, REG_X, REG_MEM, REG_ADDR); break;
  case k_opcode_t2_STY_addr: t2_strb_reg(p_buf, REG_Y, REG_MEM, REG_ADDR); break;
  case k_opcode_add_cycles:
    assert((value1 >= 0) && (value1 <= 4095));
    t2_addw(p_buf, REG_COUNTDOWN, REG_COUNTDOWN, value1);
    break;
  case k_opcode_addr_check:
    if (value2) {
      /* Writes: $8000 and up (and anything past $FFFF) need callbacks. */
      t2_shift(p_buf, k_t2_lsr, 0, REG_JIT_SCRATCH, REG_ADDR, 15);
    } else {
      /* Reads: $FC00 and up. */
      t2_addw(p_buf, REG_JIT_SCRATCH, REG_ADDR, 0x400);
      t2_shift(p_buf, k_t2_lsr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 16);
    }
    t2_emit_exit_if_scratch(p_buf, p_buf_epilog, value1);
    break;
  case k_opcode_addr_add_base_constant:
    t2_addw(p_buf, REG_ADDR, REG_ADDR_BASE, value1);
    break;
  case k_opcode_addr_add_base_y:
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_ADDR, REG_ADDR_BASE, REG_Y,
              k_t2_lsl, 0);
    break;
  case k_opcode_addr_add_x:
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_ADDR, REG_ADDR, REG_X, k_t2_lsl, 0);
    break;
  case k_opcode_addr_add_y:
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_ADDR, REG_ADDR, REG_Y, k_t2_lsl, 0);
    break;
  case k_opcode_addr_add_x_8bit:
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_ADDR, REG_ADDR, REG_X, k_t2_lsl, 0);
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_ADDR, REG_ADDR, 0xFF);
    break;
  case k_opcode_addr_add_y_8bit:
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_ADDR, REG_ADDR, REG_Y, k_t2_lsl, 0);
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_ADDR, REG_ADDR, 0xFF);
    break;
  case k_opcode_addr_set:
    t2_mov32(p_buf, REG_ADDR, (uint32_t) value1);
    break;
  case k_opcode_addr_load_8bit:
    t2_ldrb_reg(p_buf, REG_ADDR, REG_MEM, REG_ADDR);
    break;
  case k_opcode_addr_load_16bit_wrap:
    t2_emit_load_zp_pointer(p_buf, REG_ADDR);
    break;
  case k_opcode_addr_base_load_16bit_wrap:
    t2_emit_load_zp_pointer(p_buf, REG_ADDR_BASE);
    break;
  case k_opcode_addr_load_16bit_nowrap:
    /* The memory has slack past $FFFF for the high byte. */
    t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_MEM, REG_ADDR,
              k_t2_lsl, 0);
    t2_ldrb(p_buf, REG_ADDR, REG_JIT_SCRATCH, 0);
    t2_ldrb(p_buf, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_ADDR, REG_ADDR, REG_JIT_SCRATCH,
              k_t2_lsl, 8);
    break;
  case k_opcode_value_load_16bit_wrap:
    /* JMP (ind): the high byte comes from the same page. */
    t2_addw(p_buf, REG_JIT_SCRATCH, REG_ADDR, 1);
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 0xFF);
    t2_dp_imm(p_buf, k_t2_op_and, 0, REG_SCRATCH1, REG_ADDR, 0xFF00);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_SCRATCH1, k_t2_lsl, 0);
    t2_ldrb_reg(p_buf, REG_JIT_SCRATCH, REG_MEM, REG_JIT_SCRATCH);
    t2_ldrb_reg(p_buf, REG_ADDR, REG_MEM, REG_ADDR);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_ADDR, REG_ADDR, REG_JIT_SCRATCH,
              k_t2_lsl, 8);
    break;
  case k_opcode_value_set:
    t2_mov32(p_buf, REG_VALUE, (uint32_t) value1);
    break;
  case k_opcode_value_load:
    t2_ldrb_reg(p_buf, REG_VALUE, REG_MEM, REG_ADDR);
    break;
  case k_opcode_value_store:
    t2_strb_reg(p_buf, REG_VALUE, REG_MEM, REG_ADDR);
    break;
  case k_opcode_write_inv:
    /* After a store: the page's flags (bbc.h), and the write-flags stub if
     * there are any.
     */
    t2_shift(p_buf, k_t2_lsr, 0, REG_SCRATCH1, REG_ADDR, 8);
    t2_movw(p_buf, REG_JIT_SCRATCH,
            ((uint32_t) (uintptr_t) g_bbc_page_flags & 0xFFFF));
    t2_movt(p_buf, REG_JIT_SCRATCH,
            ((uint32_t) (uintptr_t) g_bbc_page_flags >> 16));
    t2_ldrb_reg(p_buf, REG_JIT_SCRATCH, REG_JIT_SCRATCH, REG_SCRATCH1);
    t2_cbz_skip(p_buf, 0, REG_JIT_SCRATCH, 4);
    t2_bl(p_buf, g_t2.p_stub_write_flags);
    break;
  case k_opcode_check_bcd:
    t2_ldst_neg(p_buf, 1, 0, REG_JIT_SCRATCH, REG_MEM, K_T2_MEM_D_FLAG);
    t2_emit_exit_if_scratch(p_buf, p_buf_epilog, value1);
    break;
  case k_opcode_check_page_crossing_n:
    t2_dp_reg(p_buf, k_t2_op_eor, 0, REG_JIT_SCRATCH, REG_ADDR_BASE, REG_ADDR,
              k_t2_lsl, 0);
    t2_emit_page_cross_refund(p_buf);
    break;
  case k_opcode_check_page_crossing_x:
  case k_opcode_check_page_crossing_y:
    t2_dp_reg(p_buf, k_t2_op_sub, 0, REG_JIT_SCRATCH, REG_ADDR,
              ((uopcode == k_opcode_check_page_crossing_x) ? REG_X : REG_Y),
              k_t2_lsl, 0);
    t2_dp_reg(p_buf, k_t2_op_eor, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_ADDR, k_t2_lsl, 0);
    t2_emit_page_cross_refund(p_buf);
    break;
  case k_opcode_check_pending_irq:
  case k_opcode_check_pending_irq_plp:
    t2_mov32(p_buf,
             REG_JIT_SCRATCH,
             (uint32_t) (uintptr_t) &g_t2.p_state_6502->abi_state.irq_fire);
    t2_ldr(p_buf, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 0);
    t2_emit_exit_if_scratch(p_buf, p_buf_epilog, value1);
    break;
  case k_opcode_countdown:
  case k_opcode_countdown_no_preserve_nz_flags:
    assert((value2 >= 0) && (value2 <= 4095));
    t2_subw(p_buf, REG_COUNTDOWN, REG_COUNTDOWN, value2);
    t2_shift(p_buf, k_t2_lsr, 0, REG_JIT_SCRATCH, REG_COUNTDOWN, 31);
    t2_emit_exit_if_scratch(p_buf, p_buf_epilog, value1);
    break;
  case k_opcode_flags_nz_a: t2_emit_nz_flags(p_buf, REG_A); break;
  case k_opcode_flags_nz_x: t2_emit_nz_flags(p_buf, REG_X); break;
  case k_opcode_flags_nz_y: t2_emit_nz_flags(p_buf, REG_Y); break;
  case k_opcode_flags_nz_value: t2_emit_nz_flags(p_buf, REG_VALUE); break;
  case k_opcode_flags_nz_mem:
    t2_emit_load_abs(p_buf, REG_JIT_SCRATCH, (uint32_t) value1);
    t2_emit_nz_flags(p_buf, REG_JIT_SCRATCH);
    break;
  case k_opcode_interp:
    t2_emit_jump_interp(p_buf, value1);
    break;
  case k_opcode_inturbo:
    /* 8 bytes: it may be written into the middle of another instruction's
     * code (a sub-instruction entry).
     */
    t2_movw(p_buf, REG_INTURBO_PC, (uint32_t) value1);
    t2_b(p_buf, g_t2.p_stub_inturbo);
    break;
  case k_opcode_JMP_SCRATCH_n:
    if (value1 != 0) {
      t2_addw(p_buf, REG_ADDR, REG_ADDR, value1);
    }
    t2_ubfx(p_buf, REG_ADDR, REG_ADDR, 0, 16);
    t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_ADDR, K_JIT_BYTES_SHIFT);
    t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
    t2_dp_imm(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              K_JIT_ADDR);
    t2_bx(p_buf, REG_JIT_SCRATCH);
    break;
  case k_opcode_load_carry:
  case k_opcode_load_overflow:
  case k_opcode_peek_to_scratch:
  case k_opcode_stack_commit_peek_increment:
    break;
  case k_opcode_PULL_16:
    t2_bl(p_buf, g_t2.p_stub_pull16);
    break;
  case k_opcode_PUSH_16:
    t2_mov32(p_buf, REG_SCRATCH1, ((uint32_t) value1 >> 8));
    t2_emit_push(p_buf, REG_SCRATCH1);
    t2_mov32(p_buf, REG_SCRATCH1, ((uint32_t) value1 & 0xFF));
    t2_emit_push(p_buf, REG_SCRATCH1);
    break;
  case k_opcode_save_carry:
    t2_emit_save_carry(p_buf);
    break;
  case k_opcode_save_overflow:
    t2_mrs_apsr(p_buf, REG_JIT_SCRATCH);
    t2_ubfx(p_buf, REG_OF, REG_JIT_SCRATCH, 28, 1);
    break;
  case k_opcode_ADC:
    /* (A << 24 | 0xFFFFFF) + (value << 24) + C: the carry in ripples into
     * bit 24, and the host C / V come out as the 6502's. N / Z are set by a
     * following flags uop.
     */
    t2_dp_imm(p_buf, k_t2_op_orn, 0, REG_JIT_SCRATCH, k_t2_pc, 0xFF000000);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, REG_A,
              k_t2_lsl, 24);
    t2_shift(p_buf, k_t2_lsr, 1, REG_SCRATCH1, REG_CF, 1);
    t2_dp_reg(p_buf, k_t2_op_adc, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_VALUE, k_t2_lsl, 24);
    t2_shift(p_buf, k_t2_lsr, 0, REG_A, REG_JIT_SCRATCH, 24);
    break;
  case k_opcode_ADD:
    t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_A, 24);
    t2_dp_reg(p_buf, k_t2_op_add, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_VALUE, k_t2_lsl, 24);
    t2_shift(p_buf, k_t2_lsr, 0, REG_A, REG_JIT_SCRATCH, 24);
    break;
  case k_opcode_SBC:
    /* A << 24 - value << 24 - !C: a borrow in ripples through the clear low
     * bits into bit 24.
     */
    t2_shift(p_buf, k_t2_lsr, 1, REG_SCRATCH1, REG_CF, 1);
    t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_A, 24);
    t2_dp_reg(p_buf, k_t2_op_sbc, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_VALUE, k_t2_lsl, 24);
    t2_shift(p_buf, k_t2_lsr, 0, REG_A, REG_JIT_SCRATCH, 24);
    break;
  case k_opcode_SUB:
    t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_A, 24);
    t2_dp_reg(p_buf, k_t2_op_sub, 1, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
              REG_VALUE, k_t2_lsl, 24);
    t2_shift(p_buf, k_t2_lsr, 0, REG_A, REG_JIT_SCRATCH, 24);
    break;
  case k_opcode_ALR:
    t2_dp_reg(p_buf, k_t2_op_and, 0, REG_A, REG_A, REG_VALUE, k_t2_lsl, 0);
    t2_shift(p_buf, k_t2_lsr, 1, REG_A, REG_A, 1);
    break;
  case k_opcode_AND:
    t2_dp_reg(p_buf, k_t2_op_and, 0, REG_A, REG_A, REG_VALUE, k_t2_lsl, 0);
    break;
  case k_opcode_EOR:
    t2_dp_reg(p_buf, k_t2_op_eor, 0, REG_A, REG_A, REG_VALUE, k_t2_lsl, 0);
    break;
  case k_opcode_ORA:
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_A, REG_A, REG_VALUE, k_t2_lsl, 0);
    break;
  case k_opcode_SAX:
    t2_dp_reg(p_buf, k_t2_op_and, 0, REG_VALUE, REG_A, REG_X, k_t2_lsl, 0);
    break;
  case k_opcode_ASL_acc:
  case k_opcode_ASL_value:
    {
      uint32_t reg = ((uopcode == k_opcode_ASL_acc) ? REG_A : REG_VALUE);
      uint32_t n = ((uopcode == k_opcode_ASL_acc) ? value1 : 1);
      assert((n >= 1) && (n <= 8));
      t2_shift(p_buf, k_t2_lsl, 0, reg, reg, 24);
      t2_shift(p_buf, k_t2_lsl, 1, reg, reg, n);
      t2_shift(p_buf, k_t2_lsr, 0, reg, reg, 24);
    }
    break;
  case k_opcode_LSR_acc:
    assert((value1 >= 1) && (value1 <= 8));
    t2_shift(p_buf, k_t2_lsr, 1, REG_A, REG_A, value1);
    break;
  case k_opcode_LSR_value:
    t2_shift(p_buf, k_t2_lsr, 1, REG_VALUE, REG_VALUE, 1);
    break;
  case k_opcode_ROL_acc:
  case k_opcode_ROR_acc:
    /* n rotates (merged by the optimizer) of the 9 bits C:A at once; REG_CF
     * is set directly (the save carry is merged, see asm_jit_rewrite).
     */
    {
      uint32_t n = value1;
      assert((n >= 1) && (n <= 8));
      if (uopcode == k_opcode_ROR_acc) {
        n = (9 - n);
      }
      t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_A, REG_CF,
                k_t2_lsl, 8);
      t2_shift(p_buf, k_t2_lsl, 0, REG_SCRATCH1, REG_JIT_SCRATCH, n);
      t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_SCRATCH1,
                REG_JIT_SCRATCH, k_t2_lsr, (9 - n));
      t2_ubfx(p_buf, REG_CF, REG_JIT_SCRATCH, 8, 1);
      t2_dp_imm(p_buf, k_t2_op_and, 0, REG_A, REG_JIT_SCRATCH, 0xFF);
    }
    break;
  case k_opcode_ROL_value: t2_emit_rol(p_buf, REG_VALUE); break;
  case k_opcode_ROR_value: t2_emit_ror(p_buf, REG_VALUE); break;
  case k_opcode_SLO:
    t2_shift(p_buf, k_t2_lsl, 0, REG_VALUE, REG_VALUE, 24);
    t2_shift(p_buf, k_t2_lsl, 1, REG_VALUE, REG_VALUE, 1);
    t2_shift(p_buf, k_t2_lsr, 0, REG_VALUE, REG_VALUE, 24);
    t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_A, REG_A, REG_VALUE, k_t2_lsl, 0);
    break;
  case k_opcode_BCC: t2_emit_branch_reg(p_buf, REG_CF, 0, (void*) value1); break;
  case k_opcode_BCS: t2_emit_branch_reg(p_buf, REG_CF, 1, (void*) value1); break;
  case k_opcode_BVC: t2_emit_branch_reg(p_buf, REG_OF, 0, (void*) value1); break;
  case k_opcode_BVS: t2_emit_branch_reg(p_buf, REG_OF, 1, (void*) value1); break;
  case k_opcode_BEQ: t2_bcond_far(p_buf, k_t2_eq, (void*) value1); break;
  case k_opcode_BNE: t2_bcond_far(p_buf, k_t2_ne, (void*) value1); break;
  case k_opcode_BMI: t2_bcond_far(p_buf, k_t2_mi, (void*) value1); break;
  case k_opcode_BPL: t2_bcond_far(p_buf, k_t2_pl, (void*) value1); break;
  case k_opcode_BIT: asm_emit_instruction_BIT_value(p_buf); break;
  case k_opcode_CLC: asm_emit_instruction_CLC(p_buf); break;
  case k_opcode_CLD: asm_emit_instruction_CLD(p_buf); break;
  case k_opcode_CLI: asm_emit_instruction_CLI(p_buf); break;
  case k_opcode_CLV: asm_emit_instruction_CLV(p_buf); break;
  case k_opcode_SEC: asm_emit_instruction_SEC(p_buf); break;
  case k_opcode_SED: asm_emit_instruction_SED(p_buf); break;
  case k_opcode_SEI: asm_emit_instruction_SEI(p_buf); break;
  case k_opcode_CMP: t2_emit_compare(p_buf, REG_A); break;
  case k_opcode_CPX: t2_emit_compare(p_buf, REG_X); break;
  case k_opcode_CPY: t2_emit_compare(p_buf, REG_Y); break;
  case k_opcode_DEC_value: t2_subw(p_buf, REG_VALUE, REG_VALUE, 1); break;
  case k_opcode_INC_value: t2_addw(p_buf, REG_VALUE, REG_VALUE, 1); break;
  case k_opcode_DEX: t2_emit_add8(p_buf, REG_X, -(int32_t) value1); break;
  case k_opcode_DEY: t2_emit_add8(p_buf, REG_Y, -(int32_t) value1); break;
  case k_opcode_INX: t2_emit_add8(p_buf, REG_X, (int32_t) value1); break;
  case k_opcode_INY: t2_emit_add8(p_buf, REG_Y, (int32_t) value1); break;
  case k_opcode_JMP: t2_b(p_buf, (void*) value1); break;
  case k_opcode_LDA: t2_mov(p_buf, REG_A, REG_VALUE); break;
  case k_opcode_LDX: t2_mov(p_buf, REG_X, REG_VALUE); break;
  case k_opcode_LDY: t2_mov(p_buf, REG_Y, REG_VALUE); break;
  case k_opcode_LDA_zero_and_flags:
  case k_opcode_LDX_zero_and_flags:
  case k_opcode_LDY_zero_and_flags:
    {
      uint32_t reg = ((uopcode == k_opcode_LDA_zero_and_flags) ? REG_A :
                      (uopcode == k_opcode_LDX_zero_and_flags) ? REG_X :
                      REG_Y);
      t2_mov32(p_buf, reg, 0);
      t2_emit_nz_flags(p_buf, reg);
    }
    break;
  case k_opcode_NOP: asm_emit_instruction_REAL_NOP(p_buf); break;
  case k_opcode_PHA: asm_emit_instruction_PHA(p_buf); break;
  case k_opcode_PHP: asm_emit_instruction_PHP(p_buf); break;
  case k_opcode_PLA: asm_emit_instruction_PLA(p_buf); break;
  case k_opcode_PLP: asm_emit_instruction_PLP(p_buf); break;
  case k_opcode_STA: t2_mov(p_buf, REG_VALUE, REG_A); break;
  case k_opcode_STX: t2_mov(p_buf, REG_VALUE, REG_X); break;
  case k_opcode_STY: t2_mov(p_buf, REG_VALUE, REG_Y); break;
  case k_opcode_TAX: asm_emit_instruction_TAX(p_buf); break;
  case k_opcode_TAY: asm_emit_instruction_TAY(p_buf); break;
  case k_opcode_TSX: asm_emit_instruction_TSX(p_buf); break;
  case k_opcode_TXA: asm_emit_instruction_TXA(p_buf); break;
  case k_opcode_TXS: asm_emit_instruction_TXS(p_buf); break;
  case k_opcode_TYA: asm_emit_instruction_TYA(p_buf); break;
  default:
    /* Encoded callbacks, debug, loop collapsing and ST_IMM are switched off
     * for this backend (see asm_jit_supports_uopcode and the jit: options the
     * embedded config sets).
     */
    log_do_log(k_log_jit, k_log_error, "thumb2: unhandled uopcode %d",
               (int) uopcode);
    assert(0);
    break;
  }
}

/* ---- Trampolines, generated into the stub page ---- */

/* The 6502 registers and flags -> state_6502 (PC not included). Uses
 * REG_ADDR as the state pointer; clobbers the scratch registers but not the
 * host flags.
 */
static void
t2_emit_save_state(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_ADDR, (uint32_t) (uintptr_t) g_t2.p_state_6502);
  t2_str(p_buf, REG_A, REG_ADDR, offsetof(struct state_6502, abi_state.reg_a));
  t2_str(p_buf, REG_X, REG_ADDR, offsetof(struct state_6502, abi_state.reg_x));
  t2_str(p_buf, REG_Y, REG_ADDR, offsetof(struct state_6502, abi_state.reg_y));
  t2_str(p_buf, REG_S, REG_ADDR, offsetof(struct state_6502, abi_state.reg_s));
  t2_str(p_buf, REG_VALUE, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_host_value));
  t2_emit_flags_to_scratch1(p_buf);
  t2_str(p_buf, REG_SCRATCH1, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_flags));
}

/* state_6502 -> the 6502 registers and flags. Clobbers scratch and REG_ADDR. */
static void
t2_emit_restore_state(struct util_buffer* p_buf) {
  t2_mov32(p_buf, REG_ADDR, (uint32_t) (uintptr_t) g_t2.p_state_6502);
  t2_ldr(p_buf, REG_A, REG_ADDR, offsetof(struct state_6502, abi_state.reg_a));
  t2_ldr(p_buf, REG_X, REG_ADDR, offsetof(struct state_6502, abi_state.reg_x));
  t2_ldr(p_buf, REG_Y, REG_ADDR, offsetof(struct state_6502, abi_state.reg_y));
  t2_ldr(p_buf, REG_S, REG_ADDR, offsetof(struct state_6502, abi_state.reg_s));
  t2_ldr(p_buf, REG_VALUE, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_host_value));
  t2_ldr(p_buf, REG_SCRATCH1, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_flags));
  t2_emit_flags_from_scratch1(p_buf);
}

/* lr = the compile stub; jump to the JIT code for state_6502's PC. */
static void
t2_emit_jump_to_state_pc(struct util_buffer* p_buf) {
  t2_mov32(p_buf, k_t2_lr, ((uint32_t) (uintptr_t) g_t2.p_stub_compile | 1));
  t2_mov32(p_buf, REG_ADDR, (uint32_t) (uintptr_t) g_t2.p_state_6502);
  t2_ldr(p_buf, REG_JIT_SCRATCH, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_pc));
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
           K_JIT_BYTES_SHIFT);
  t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
  t2_dp_imm(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
            K_JIT_ADDR);
  t2_bx(p_buf, REG_JIT_SCRATCH);
}

/* r0 = *p_ptr_to_ptr, as a constant load then a load. */
static void
t2_emit_load_indirect(struct util_buffer* p_buf, uint32_t reg, void* p) {
  t2_mov32(p_buf, reg, (uint32_t) (uintptr_t) p);
  t2_ldr(p_buf, reg, reg, 0);
}

/* Return from a bl helper: lr back to the compile stub, flags untouched. */
static void
t2_emit_helper_return(struct util_buffer* p_buf) {
  t2_mov(p_buf, REG_JIT_SCRATCH, k_t2_lr);
  t2_mov32(p_buf, k_t2_lr, ((uint32_t) (uintptr_t) g_t2.p_stub_compile | 1));
  t2_bx(p_buf, REG_JIT_SCRATCH);
}

/* Start the stub at its fixed offset (thumb2_defs.h), filling the gap with
 * traps.
 */
static void
t2_stub_at(struct util_buffer* p_buf, uint32_t offset, uint8_t* p_expect) {
  size_t pos = util_buffer_get_pos(p_buf);
  if (pos > offset) {
    util_bail("thumb2 JIT: stub before 0x%x overruns (0x%x)",
              (unsigned) offset, (unsigned) pos);
  }
  while (pos < offset) {
    t2_udf(p_buf, 0);
    pos += 2;
  }
  assert(p_expect == ((uint8_t*) K_T2_STUB_ADDR + offset));
  (void) p_expect;
}

/* Into the interpreter, with the 6502 PC in REG_JIT_SCRATCH: callback(object,
 * p_ret, countdown, host_flags), p_ret being two int64s (the new countdown,
 * and whether the CPU exited); then on in JIT code at the PC it stopped at.
 */
static void
t2_emit_interp_stub(struct util_buffer* p_buf,
                    void** pp_object,
                    void** pp_callback,
                    uint8_t* p_exit) {
  t2_mov32(p_buf, REG_ADDR, (uint32_t) (uintptr_t) g_t2.p_state_6502);
  t2_str(p_buf, REG_JIT_SCRATCH, REG_ADDR,
         offsetof(struct state_6502, abi_state.reg_pc));
  t2_emit_save_state(p_buf);
  t2_mrs_apsr(p_buf, REG_SCRATCH2);
  /* sub sp, #24: host_flags at sp, the return values at sp + 8. */
  t2_h(p_buf, 0xB086);
  t2_mov32(p_buf, k_t2_r3, 0);
  t2_str(p_buf, REG_SCRATCH2, k_t2_sp, 0);
  t2_str(p_buf, k_t2_r3, k_t2_sp, 4);
  t2_emit_load_indirect(p_buf, k_t2_r0, pp_object);
  /* add r1, sp, #8 */
  t2_dp_imm(p_buf, k_t2_op_add, 0, k_t2_r1, k_t2_sp, 8);
  t2_mov(p_buf, k_t2_r2, REG_COUNTDOWN);
  t2_shift(p_buf, k_t2_asr, 0, k_t2_r3, REG_COUNTDOWN, 31);
  t2_emit_load_indirect(p_buf, REG_ADDR, pp_callback);
  t2_blx(p_buf, REG_ADDR);
  t2_ldr(p_buf, REG_COUNTDOWN, k_t2_sp, 8);
  t2_ldr(p_buf, REG_JIT_SCRATCH, k_t2_sp, 16);
  /* add sp, #24 */
  t2_h(p_buf, 0xB006);
  t2_cbz_skip(p_buf, 0, REG_JIT_SCRATCH, 4);
  t2_b(p_buf, p_exit);
  t2_emit_restore_state(p_buf);
  t2_emit_jump_to_state_pc(p_buf);
}

static void
t2_generate_stubs(void) {
  struct util_buffer* p_buf = util_buffer_create();
  uint8_t* p_stubs = (uint8_t*) K_T2_STUB_ADDR;
  uint8_t* p_exit;

  util_buffer_setup(p_buf, p_stubs, K_T2_STUB_SIZE);
  asm_fill_with_trap(p_buf);
  util_buffer_set_pos(p_buf, 0);

  /* The compile stub's address is needed by the others: lay out compile
   * first.
   *
   * Compile: entered by "blx lr" from invalidated code, lr = that code + 2,
   * with bit 0 set. jit_compile(p_jit, p_host_pc, countdown, host_flags)
   * returns the new countdown and leaves the PC to resume at in state_6502.
   */
  t2_stub_at(p_buf, K_T2_STUB_COMPILE, g_t2.p_stub_compile);
  t2_emit_save_state(p_buf);
  t2_mrs_apsr(p_buf, REG_SCRATCH2);
  t2_mov32(p_buf, k_t2_r3, 0);
  /* sub sp, #8; host_flags (64-bit) on the stack. */
  t2_h(p_buf, 0xB082);
  t2_str(p_buf, REG_SCRATCH2, k_t2_sp, 0);
  t2_str(p_buf, k_t2_r3, k_t2_sp, 4);
  t2_mov32(p_buf, k_t2_r0, (uint32_t) (uintptr_t) g_t2.p_context);
  t2_subw(p_buf, k_t2_r1, k_t2_lr, 3);
  t2_mov(p_buf, k_t2_r2, REG_COUNTDOWN);
  t2_shift(p_buf, k_t2_asr, 0, k_t2_r3, REG_COUNTDOWN, 31);
  t2_emit_load_indirect(p_buf, REG_ADDR, g_t2.p_compile_callback);
  t2_blx(p_buf, REG_ADDR);
  /* add sp, #8 */
  t2_h(p_buf, 0xB002);
  t2_mov(p_buf, REG_COUNTDOWN, k_t2_r0);
  t2_emit_restore_state(p_buf);
  t2_emit_jump_to_state_pc(p_buf);

  /* Helpers, called with bl from JIT code: on the way out, lr goes back to
   * the compile stub.
   */
  t2_stub_at(p_buf, K_T2_STUB_PHP, g_t2.p_stub_php);
  t2_emit_php_body(p_buf);
  t2_emit_helper_return(p_buf);
  t2_stub_at(p_buf, K_T2_STUB_PLP, g_t2.p_stub_plp);
  t2_emit_plp_body(p_buf);
  t2_emit_helper_return(p_buf);
  /* REG_ADDR = a 16-bit word pulled from the stack (RTS / RTI). */
  t2_stub_at(p_buf, K_T2_STUB_PULL16, g_t2.p_stub_pull16);
  t2_addw(p_buf, REG_JIT_SCRATCH, REG_S, 1);
  t2_dp_imm(p_buf, k_t2_op_and, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 0xFF);
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_MEM,
            REG_JIT_SCRATCH, k_t2_lsl, 0);
  t2_ldrb(p_buf, REG_SCRATCH1, REG_JIT_SCRATCH, 0x100);
  t2_emit_add8(p_buf, REG_S, 2);
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_MEM, REG_S,
            k_t2_lsl, 0);
  t2_ldrb(p_buf, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 0x100);
  t2_dp_reg(p_buf, k_t2_op_orr, 0, REG_ADDR, REG_SCRATCH1, REG_JIT_SCRATCH,
            k_t2_lsl, 8);
  t2_emit_helper_return(p_buf);

  /* Exit: back to asm_jit_enter's caller with 1 (exited). */
  p_exit = g_t2.p_stub_exit;
  t2_stub_at(p_buf, K_T2_STUB_EXIT, p_exit);
  t2_mov32(p_buf, k_t2_r0, 1);
  /* pop {r3-r11, pc} */
  t2_w(p_buf, 0xE8BD, 0x8FF8);

  /* Interp: entered by a branch, with the 6502 PC in REG_JIT_SCRATCH.
   * jit_enter_interp(p_object, p_ret, countdown, host_flags), with p_ret two
   * int64s: the new countdown, and whether the CPU exited.
   */
  t2_stub_at(p_buf, K_T2_STUB_INTERP, g_t2.p_stub_interp);
  t2_emit_interp_stub(p_buf, g_t2.p_interp_object, g_t2.p_interp_callback,
                      p_exit);

  /* Harness only: flush the invalidated code at REG_JIT_SCRATCH unless it's
   * the no-code page (the Linux ARM cacheflush syscall), preserving
   * everything including the flags.
   */
  t2_stub_at(p_buf, K_T2_STUB_FLUSH_INV, g_t2.p_stub_flush_inv);
  /* push {r0-r4, r7, r12, lr} */
  t2_w(p_buf, 0xE92D, 0x509F);
  t2_mrs_apsr(p_buf, REG_CF);
  t2_mov(p_buf, k_t2_r0, REG_JIT_SCRATCH);
  t2_mov32(p_buf, k_t2_r1, K_JIT_NO_CODE_JIT_PTR_PAGE);
  t2_dp_reg(p_buf, k_t2_op_sub, 0, k_t2_r1, k_t2_r0, k_t2_r1, k_t2_lsl, 0);
  t2_ubfx(p_buf, k_t2_r1, k_t2_r1, 12, 20);
  /* Skip the syscall if r0 is in the no-code page. */
  t2_cbz_skip(p_buf, 0, k_t2_r1, 18);
  t2_dp_imm(p_buf, k_t2_op_add, 0, k_t2_r1, k_t2_r0, 4);
  t2_mov32(p_buf, k_t2_r2, 0);
  t2_movw(p_buf, REG_JIT_SCRATCH, 0x0002);
  t2_movt(p_buf, REG_JIT_SCRATCH, 0x000f);
  /* svc 0 */
  t2_h(p_buf, 0xDF00);
  t2_msr_apsr(p_buf, REG_CF);
  /* pop {r0-r4, r7, r12, lr}, then back with lr = the compile stub again (the
   * bl here replaced it).
   */
  t2_w(p_buf, 0xE8BD, 0x509F);
  t2_emit_helper_return(p_buf);

  /* Interp from JIT code: "bl" here with the 6502 PC in the halfword after
   * the bl (lr - 1 is its address); on into the interp stub.
   */
  t2_stub_at(p_buf, K_T2_STUB_INTERP_INLINE, g_t2.p_stub_interp_inline);
  t2_ldst_neg(p_buf, 1, 1, REG_JIT_SCRATCH, k_t2_lr, 1);
  t2_b(p_buf, g_t2.p_stub_interp);

  /* Enter: called from C as (p_code, countdown, p_mem). */
  t2_stub_at(p_buf, K_T2_STUB_ENTER, g_t2.p_stub_enter);
  /* push {r3-r11, lr}: ten registers keeps the stack 8-byte aligned. */
  t2_w(p_buf, 0xE92D, 0x4FF8);
  t2_mov(p_buf, REG_COUNTDOWN, k_t2_r1);
  t2_mov(p_buf, REG_MEM, k_t2_r2);
  /* The target (with the Thumb bit) waits on the stack while restore_state
   * uses every register.
   */
  t2_dp_imm(p_buf, k_t2_op_orr, 0, k_t2_r0, k_t2_r0, 1);
  /* push {r0, r1} (r1 for alignment) */
  t2_h(p_buf, 0xB403);
  t2_emit_restore_state(p_buf);
  t2_mov32(p_buf, k_t2_lr, ((uint32_t) (uintptr_t) g_t2.p_stub_compile | 1));
  /* pop {r7, r8}: the target into REG_JIT_SCRATCH. */
  t2_w(p_buf, 0xE8BD, 0x0180);
  t2_bx(p_buf, REG_JIT_SCRATCH);

  /* Into inturbo: the handler for the opcode at REG_INTURBO_PC. */
  t2_stub_at(p_buf, K_T2_STUB_INTURBO, g_t2.p_stub_inturbo);
  t2_ldrb_reg(p_buf, REG_JIT_SCRATCH, REG_MEM, REG_INTURBO_PC);
  t2_mov32(p_buf, REG_SCRATCH1, ((uint32_t) K_INTURBO_ADDR | 1));
  t2_dp_reg(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_SCRATCH1,
            REG_JIT_SCRATCH, k_t2_lsl, K_INTURBO_OPCODE_SHIFT);
  t2_bx(p_buf, REG_JIT_SCRATCH);

  /* Out of inturbo: to the JIT code for REG_INTURBO_PC, with lr = the compile
   * stub again (a handler may use it). Flags untouched.
   */
  t2_stub_at(p_buf, K_T2_STUB_INTURBO_EXIT, g_t2.p_stub_inturbo_exit);
  t2_ubfx(p_buf, REG_INTURBO_PC, REG_INTURBO_PC, 0, 16);
  t2_shift(p_buf, k_t2_lsl, 0, REG_JIT_SCRATCH, REG_INTURBO_PC,
           K_JIT_BYTES_SHIFT);
  t2_dp_imm(p_buf, k_t2_op_orr, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH, 1);
  t2_dp_imm(p_buf, k_t2_op_add, 0, REG_JIT_SCRATCH, REG_JIT_SCRATCH,
            K_JIT_ADDR);
  t2_mov32(p_buf, k_t2_lr, ((uint32_t) (uintptr_t) g_t2.p_stub_compile | 1));
  t2_bx(p_buf, REG_JIT_SCRATCH);

  /* Inturbo's way into the interpreter: as the interp stub, but through
   * inturbo's callback, which skips the JIT's stale state fixups (inturbo's
   * state is always exact, and a sub-instruction PC has none).
   */
  t2_stub_at(p_buf, K_T2_STUB_INTURBO_INTERP, g_t2.p_stub_inturbo_interp);
  if (g_t2.p_inturbo_interp_object != NULL) {
    t2_emit_interp_stub(p_buf, g_t2.p_inturbo_interp_object,
                        g_t2.p_inturbo_interp_callback, p_exit);
  }
  t2_stub_at(p_buf, K_T2_STUB_WRITE_FLAGS, g_t2.p_stub_write_flags);
  t2_emit_write_flags_stub(p_buf, REG_ADDR, 0);
  t2_stub_at(p_buf, K_T2_STUB_INTURBO_WRITE_FLAGS,
             g_t2.p_stub_inturbo_write_flags);
  t2_emit_write_flags_stub(p_buf, REG_INTURBO_EA, 1);

  t2_stub_at(p_buf, K_T2_STUB_END, ((uint8_t*) K_T2_STUB_ADDR + K_T2_STUB_END));
  assert(util_buffer_get_pos(p_buf) <= K_T2_STUB_SIZE);
  t2_flush(p_stubs, K_T2_STUB_SIZE);
  util_buffer_destroy(p_buf);
}

static void
t2_setup(void* p_context) {
  struct cpu_driver* p_driver = (struct cpu_driver*) p_context;
  struct t2_jit_head* p_head = (struct t2_jit_head*) p_context;

  g_t2.p_context = p_context;
  g_t2.p_state_6502 = p_driver->abi.p_state_6502;
  g_t2.p_mem = p_driver->p_extra->p_memory_access->p_mem_read;
  g_t2.p_jit_ptrs = &p_head->jit_ptrs[0];
  g_t2.p_compile_callback = &p_head->p_compile_callback;
  g_t2.p_interp_callback = &p_driver->abi.p_interp_callback;
  g_t2.p_interp_object = &p_driver->abi.p_interp_object;
  if (p_head->p_inturbo != NULL) {
    struct cpu_driver* p_inturbo = (struct cpu_driver*) p_head->p_inturbo;
    g_t2.p_inturbo_interp_callback = &p_inturbo->abi.p_interp_callback;
    g_t2.p_inturbo_interp_object = &p_inturbo->abi.p_interp_object;
  }
  t2_layout_stubs();
  t2_generate_stubs();
  /* Inturbo's handlers were written at JIT creation. */
  t2_flush((void*) K_INTURBO_ADDR, K_INTURBO_SIZE);
}

uint32_t
asm_jit_enter(void* p_context,
              void* p_start_addr,
              int64_t countdown,
              void* p_mem_base) {
  uint32_t (*p_enter)(void*, int32_t, uint8_t*);
  (void) p_mem_base;

  if (g_t2.p_context == NULL) {
    t2_setup(p_context);
  }
  assert(p_context == g_t2.p_context);
  assert((countdown >= INT32_MIN) && (countdown <= INT32_MAX));

  p_enter = (uint32_t (*)(void*, int32_t, uint8_t*))
                ((uintptr_t) g_t2.p_stub_enter | 1);
  return p_enter(p_start_addr, (int32_t) countdown, g_t2.p_mem);
}

/* jit.c puts this in the ABI struct; JIT code reaches the generated interp
 * stub directly instead.
 */
void
asm_jit_interp_trampoline(void) {
  assert(0);
}
