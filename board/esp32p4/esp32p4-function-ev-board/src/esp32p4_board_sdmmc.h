/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_board_sdmmc.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_BOARD_SDMMC_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_BOARD_SDMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_SDMMC

/****************************************************************************
 * Name: board_sdmmc_initialize
 *
 * Description:
 *   Initialize SDMMC subsystem for the ESP32-P4 Function EV Board.
 *
 *   - Slot 0: External TF card (IOMUX pins) -> /dev/mmcsd0
 *   - Slot 1: ESP-Hosted SDIO to ESP32-C6 (GPIO matrix pins)
 *
 * Returned Value:
 *   Zero (OK) on success; negated errno on failure.
 *
 ****************************************************************************/

int board_sdmmc_initialize(void);

#endif /* CONFIG_ESP32P4_SDMMC */
#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_BOARD_SDMMC_H */
