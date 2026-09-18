/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_sdmmc.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_SDMMC_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_SDMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/
#include <nuttx/sdio.h>

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Pre-processor definitions
 ****************************************************************************/

/****************************************************************************
 * Public Functions Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: sdio_initialize
 *
 * Description:
 *   Initialize SDIO for operation.
 *
 * Input Parameters:
 *   slotno - SDMMC slot number (0 for IOMUX, 1 for GPIO matrix).
 *
 * Returned Value:
 *   A reference to an SDIO interface structure.  NULL is returned on
 *   failures.
 *
 ****************************************************************************/

struct sdio_dev_s *sdio_initialize(int slotno);

/****************************************************************************
 * Name: esp32p4_sdio_wait_card_interrupt
 *
 * Description:
 *   Wait for an SDIO function interrupt on DAT1.  NuttX's generic SDIO
 *   event set has no card-interrupt event, so ESP32-P4 users that need SDIO
 *   I/O interrupts use this controller-specific entry point.
 *
 ****************************************************************************/

int esp32p4_sdio_wait_card_interrupt(struct sdio_dev_s *dev,
                                     uint32_t timeout_ticks);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_SDMMC_H */
