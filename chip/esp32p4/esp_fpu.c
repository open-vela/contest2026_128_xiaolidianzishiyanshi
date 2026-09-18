/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_fpu.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Work around the ESP32-P4 revision 0.x/1.x EXT_ILL erratum.  On affected
 * silicon the coprocessor illegal-instruction reason CSR does not identify
 * FLW/FSW (including their compressed forms) as FPU instructions.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <arch/csr.h>
#include <arch/irq.h>
#include <arch/mode.h>

#include "riscv_internal.h"

#include "esp_fpu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32P4_EXT_ILL_RSN_FPU    (1 << 0)

#define RISCV_OPCODE_MASK          0x7f
#define RISCV_OPCODE_FP_LOAD       0x07
#define RISCV_OPCODE_FP_STORE      0x27
#define RISCV_OPCODE_SYSTEM        0x73

#define RISCV_C_FP_MASK            0x6001
#define RISCV_C_FP_VALUE           0x6000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#if defined(CONFIG_ARCH_FPU) && \
    defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)

static inline uint32_t esp_fpu_get_and_clear_ext_ill(void)
{
  uint32_t reason;

  __asm__ __volatile__("csrrw %0, 0x7f0, zero" : "=r"(reason));
  return reason;
}

static bool esp_fpu_instruction(uint32_t instruction, uint32_t reason)
{
  uint32_t opcode = instruction & RISCV_OPCODE_MASK;
  uint32_t csr;

  /* EXT_ILL correctly reports arithmetic FPU instructions.  The remaining
   * checks mirror ESP-IDF's fallback for the load/store reporting erratum.
   */

  if ((reason & ESP32P4_EXT_ILL_RSN_FPU) != 0)
    {
      return true;
    }

  if (opcode == RISCV_OPCODE_FP_LOAD || opcode == RISCV_OPCODE_FP_STORE)
    {
      return true;
    }

  /* RV32 C.FLW/C.FSW and C.FLWSP/C.FSWSP all have instruction bits
   * [14:13] set and bit zero clear.
   */

  if ((instruction & RISCV_C_FP_MASK) == RISCV_C_FP_VALUE)
    {
      return true;
    }

  /* FFLAGS, FRM and FCSR are CSRs 1, 2 and 3 respectively. */

  if (opcode == RISCV_OPCODE_SYSTEM)
    {
      csr = (instruction >> 20) & 0xfff;
      return csr >= 1 && csr <= 3;
    }

  return false;
}

static int esp_fpu_exception(int irq, void *context, void *arg)
{
  uintreg_t *regs = context;
  uintreg_t status = regs[REG_INT_CTX];
  uint32_t instruction = READ_CSR(CSR_TVAL);
  uint32_t reason = esp_fpu_get_and_clear_ext_ill();
  bool first_use = (status & MSTATUS_FS) == 0;

  /* ESP32-P4 revision 0.x/1.x may raise EXT_ILL for the first FPU use even
   * when the architectural FS field is already non-zero.  ESP-IDF handles
   * this by writing the FPU-enable bit to mstatus unconditionally and then
   * retrying the instruction.  In particular, FS being Dirty is not proof
   * that this is a genuine illegal instruction.
   */

  if (!esp_fpu_instruction(instruction, reason))
    {
      return riscv_exception(irq, context, arg);
    }

  /* New tasks enter with FS=Off, matching IDF's lazy coprocessor
   * lifecycle.  Enable the live FPU and the saved return context, then retry
   * the faulting instruction.  The normal NuttX save path owns the register
   * contents from the point at which the retried instruction marks FS Dirty.
   */

  SET_CSR(CSR_STATUS, MSTATUS_FS_INIT);
  if (first_use)
    {
      /* A task with no saved FPU context must not inherit an arbitrary frm
       * value from the core.  Reserved frm values (5..7) make otherwise
       * valid dynamic-rounding instructions raise Illegal Instruction.
       */

      WRITE_CSR(CSR_FCSR, 0);
    }

  status |= MSTATUS_FS_INIT;
  regs[REG_INT_CTX] = status;

  return OK;
}
#endif

void __real_riscv_restorefpu(uintreg_t *regs, uintreg_t *fregs);

void __wrap_riscv_restorefpu(uintreg_t *regs, uintreg_t *fregs)
{
#ifdef CONFIG_ARCH_FPU
  uintreg_t status = regs[REG_INT_CTX] & MSTATUS_FS;

  /* The real riscv_restorefpu() loads registers for Clean/Dirty saved
   * contexts.  The task we are switching away from may have FS=Off, so
   * enable the live FPU before those loads.  This context-switch ordering
   * applies to every ESP32-P4 revision; only the illegal-instruction
   * erratum handler below is restricted to pre-v3 silicon.
   */

  if (status > MSTATUS_FS_INIT)
    {
      SET_CSR(CSR_STATUS, MSTATUS_FS_INIT);
    }
#else
  (void)regs;
#endif

  __real_riscv_restorefpu(regs, fregs);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void esp_fpu_exception_initialize(void)
{
#if defined(CONFIG_ARCH_FPU) && \
    defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
  /* NuttX's non-lazy context switch restores FPU registers while still in
   * exception context.  Enable the live FPU here, after the application
   * image has been mapped but before nx_start creates/switches tasks.
   * Calling this from the earlier ROM/boot path is unsafe because this
   * routine itself resides in the application IROM mapping.
   */

  riscv_fpuconfig();

  /* Override only the illegal-instruction slot installed by
   * riscv_exception_attach().  All unrecognized instructions are forwarded
   * to the original NuttX exception handler.
   */

  irq_attach(RISCV_IRQ_IINSTRUCTION, esp_fpu_exception, NULL);
#endif
}
