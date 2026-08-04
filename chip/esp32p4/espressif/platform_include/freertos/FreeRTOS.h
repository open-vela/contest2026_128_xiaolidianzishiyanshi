/****************************************************************************
 * arch/risc-v/src/esp32p4/platform_include/freertos/FreeRTOS.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_PLATFORM_INCLUDE_FREERTOS_FREERTOS_H
#define __ARCH_RISCV_SRC_ESP32P4_PLATFORM_INCLUDE_FREERTOS_FREERTOS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "platform/os.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A small compatibility surface used by ESP-IDF upper HAL components. */

typedef rspinlock_t portMUX_TYPE;

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define portMUX_INITIALIZE(lock) esp_os_spinlock_initialize(lock)
#define portYIELD_FROM_ISR() OS_PORT_YIELD_FROM_ISR()

#endif
