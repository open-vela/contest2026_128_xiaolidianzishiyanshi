/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_fpu.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integrate the ESP32-P4 FPU EXT_ILL lifecycle with NuttX.  All revisions
 * report disabled/invalid FPU use through EXT_ILL; revisions older than 3.0
 * additionally need opcode fallback because FLW/FSW may omit the reason bit.
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

#ifdef CONFIG_ARCH_FPU

static inline uint32_t esp_fpu_get_and_clear_ext_ill(void)
{
  uint32_t reason;

  __asm__ __volatile__("csrrw %0, 0x7f0, zero" : "=r"(reason));
  return reason;
}

static bool esp_fpu_instruction(uint32_t instruction, uint32_t reason)
{
#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  uint32_t opcode = instruction & RISCV_OPCODE_MASK;
  uint32_t csr;
#endif

  /* EXT_ILL reports FPU instructions on every ESP32-P4 revision.  The
   * remaining opcode checks mirror ESP-IDF's fallback for the load/store
   * reporting erratum, which was fixed in revision 3.0.
   */

  if ((reason & ESP32P4_EXT_ILL_RSN_FPU) != 0)
    {
      return true;
    }

#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
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
#else
  (void)instruction;
#endif

  return false;
}

static int esp_fpu_exception(int irq, void *context, void *arg)
{
  uintreg_t *regs = context;
  uintreg_t status = regs[REG_INT_CTX];
  uint32_t instruction = READ_CSR(CSR_TVAL);
  uint32_t reason = esp_fpu_get_and_clear_ext_ill();
  bool initialize_fcsr = (status & MSTATUS_FS) <= MSTATUS_FS_INIT;

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

  /* Legacy tasks enter with FS=Off for IDF-compatible lazy startup; normal
   * NuttX tasks enter with FS=Initial.  Enable the live FPU and saved return
   * context, then retry the faulting instruction.  The normal NuttX save
   * path owns the registers once the retried instruction marks FS Dirty.
   */

  SET_CSR(CSR_STATUS, MSTATUS_FS_INIT);
  if (initialize_fcsr)
    {
      /* A task with no saved FPU context must not inherit an arbitrary frm
       * value from the core.  Reserved frm values (5..7) make otherwise
       * valid dynamic-rounding instructions raise Illegal Instruction.  A
       * new non-lazy NuttX context starts at FS=Initial, while the legacy
       * ESP32-P4 lazy path starts at FS=Off, so handle both states.
       */

      WRITE_CSR(CSR_FCSR, 0);
    }

  status |= MSTATUS_FS_INIT;
  regs[REG_INT_CTX] = status;

  return OK;
}
#endif /* CONFIG_ARCH_FPU */

void __real_riscv_restorefpu(uintreg_t *regs, uintreg_t *fregs);

void __wrap_riscv_restorefpu(uintreg_t *regs, uintreg_t *fregs)
{
#ifdef CONFIG_ARCH_FPU
  uintreg_t status = regs[REG_INT_CTX] & MSTATUS_FS;

  /* The real riscv_restorefpu() loads registers for Clean/Dirty saved
   * contexts.  The task we are switching away from may have FS=Off, so
   * enable the live FPU before those loads.  This context-switch ordering
   * applies to every ESP32-P4 revision.
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
#ifdef CONFIG_ARCH_FPU
  /* NuttX's non-lazy context switch restores FPU registers while still in
   * exception context.  Enable the live FPU here, after the application
   * image has been mapped but before nx_start creates/switches tasks.
   * Calling this from the earlier ROM/boot path is unsafe because this
   * routine itself resides in the application IROM mapping.  Every ESP32-P4
   * revision needs this architectural FPU initialization.
   */

  riscv_fpuconfig();

  /* ESP-IDF handles the EXT_ILL FPU reason on every ESP32-P4 revision.  Only
   * the opcode fallback inside esp_fpu_instruction() is restricted to chips
   * older than v3.0.  Override only the illegal-instruction slot installed
   * by riscv_exception_attach(); all unrecognized instructions are forwarded
   * to the original NuttX exception handler.
   */

  irq_attach(RISCV_IRQ_IINSTRUCTION, esp_fpu_exception, NULL);
#endif /* CONFIG_ARCH_FPU */
}
