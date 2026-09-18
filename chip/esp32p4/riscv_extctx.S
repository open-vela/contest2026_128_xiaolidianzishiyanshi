/****************************************************************************
 * arch/risc-v/src/esp32p4/riscv_extctx.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* ESP32-P4 revisions before v3 implement the draft CLIC return semantics:
 * mcause.MPIL is copied back to mintstatus.MIL by mret.  Preserve the full
 * mcause value as one NuttX integer-context extension so an interrupt return
 * (including one which switches tasks) restores the interrupted priority.
 * On v3.x the extension hooks are empty, even if the extension option was
 * retained when switching away from legacy revisions.
 */

#define ESP32P4_REG_MCAUSE (REG_INT_CTX + INT_REG_SIZE)

.macro save_extctx base
#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  csrr      t0, CSR_CAUSE
  REGSTORE  t0, ESP32P4_REG_MCAUSE(\base)
#endif
.endm

.macro load_extctx base
#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  REGLOAD   t0, ESP32P4_REG_MCAUSE(\base)
  csrw      CSR_CAUSE, t0
#endif
.endm
