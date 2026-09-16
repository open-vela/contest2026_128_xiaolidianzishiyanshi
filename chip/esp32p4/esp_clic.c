/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_clic.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/sched.h>

#include <arch/mode.h>

#include "riscv_internal.h"

#if defined(CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS) && \
    defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32P4_MCAUSE_MPP_M          (3u << 28)
#define ESP32P4_REG_MCAUSE_NDX        (REG_INT_CTX_NDX + 1)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void riscv_initial_extctx_state(struct tcb_s *tcb)
{
  /* Draft CLIC stores both MPIL and MPP in mcause.  New NuttX tasks need
   * MPIL=0 so normal interrupts are accepted, but MPP must be M-mode or the
   * first mret into the task would enter U-mode and privileged CSR accesses
   * (for example up_irq_save() touching mstatus) would fault.
   */

  tcb->xcp.regs[ESP32P4_REG_MCAUSE_NDX] = ESP32P4_MCAUSE_MPP_M;

#ifdef CONFIG_ARCH_FPU
  /* Match ESP-IDF's lazy coprocessor startup: a new task must enter with the
   * FPU disabled so its first FPU instruction reaches the EXT_ILL hook.
   */

  tcb->xcp.regs[REG_INT_CTX] &= ~MSTATUS_FS;
#endif
}

#endif
