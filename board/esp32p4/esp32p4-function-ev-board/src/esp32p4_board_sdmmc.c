/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_board_sdmmc.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sys/types.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/mmcsd.h>
#include <nuttx/sdio.h>

#include <arch/chip/gpio_sig_map.h>

#include "espressif/esp_gpio.h"
#include "esp_ldo_regulator.h"
#include "esp32p4_sdmmc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Slot 1 GPIO matrix pin assignments from Kconfig */

#define BOARD_SDMMC_SLOT1_CLK  CONFIG_ESP32P4_SDMMC_SLOT1_PIN_CLK
#define BOARD_SDMMC_SLOT1_CMD  CONFIG_ESP32P4_SDMMC_SLOT1_PIN_CMD
#define BOARD_SDMMC_SLOT1_D0   CONFIG_ESP32P4_SDMMC_SLOT1_PIN_D0
#define BOARD_SDMMC_SLOT1_D1   CONFIG_ESP32P4_SDMMC_SLOT1_PIN_D1
#define BOARD_SDMMC_SLOT1_D2   CONFIG_ESP32P4_SDMMC_SLOT1_PIN_D2
#define BOARD_SDMMC_SLOT1_D3   CONFIG_ESP32P4_SDMMC_SLOT1_PIN_D3

/* The Function-EV-Board routes the slot 0 I/O bank and TF-card supply to
 * the ESP32-P4 internal GP LDO VO4.  Keep the handle for the lifetime of
 * the SDMMC device so that the regulator remains enabled.
 */

#ifdef CONFIG_ESP32P4_SDMMC_SLOT0
#  define BOARD_SDMMC_LDO_CHANNEL  4
#  define BOARD_SDMMC_LDO_MV       3300

static esp_ldo_channel_handle_t g_sdmmc_ldo;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_SDMMC_SLOT0
static int esp32p4_sdmmc_slot0_enable_power(void)
{
  const esp_ldo_channel_config_t config =
  {
    .chan_id = BOARD_SDMMC_LDO_CHANNEL,
    .voltage_mv = BOARD_SDMMC_LDO_MV,
  };

  int ret;

  if (g_sdmmc_ldo != NULL)
    {
      return OK;
    }

  ret = esp_ldo_acquire_channel(&config, &g_sdmmc_ldo);
  if (ret != 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to enable SDMMC VO4 LDO: %d\n", ret);
      return -EIO;
    }

  /* Allow the TF-card supply and the I/O rail to settle before any pin is
   * driven or the controller emits the initial clock cycles.
   */

  up_mdelay(10);
  syslog(LOG_INFO, "SDMMC slot 0 VO4 LDO enabled at %d mV\n",
         BOARD_SDMMC_LDO_MV);
  return OK;
}
#endif

#ifdef CONFIG_ESP32P4_SDMMC_SLOT1
/****************************************************************************
 * Name: esp32p4_sdmmc_slot1_configure_pins
 *
 * Description:
 *   Configure GPIO matrix pins for SDMMC slot 1 (ESP-Hosted to C6).
 *
 *   Slot 1 on ESP32-P4 uses GPIO matrix routing, not IOMUX.
 *   Pin numbers come from Kconfig (CONFIG_ESP32P4_SDMMC_SLOT1_PIN_*).
 *
 ****************************************************************************/

static void esp32p4_sdmmc_slot1_configure_pins(void)
{
  /* CLK: output only */

  esp_configgpio(BOARD_SDMMC_SLOT1_CLK, OUTPUT_FUNCTION_1);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_CLK,
                       SD_CARD_CCLK_2_PAD_OUT_IDX, false, false);

  /* CMD: bidirectional, open-drain, pull-up */

  esp_configgpio(BOARD_SDMMC_SLOT1_CMD,
                 OUTPUT_FUNCTION_1 | INPUT_FUNCTION_1 | OPEN_DRAIN | PULLUP);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_CMD,
                       SD_CARD_CCMD_2_PAD_OUT_IDX, false, false);
  esp_gpio_matrix_in(BOARD_SDMMC_SLOT1_CMD,
                      SD_CARD_CCMD_2_PAD_IN_IDX, false);

  /* D0: bidirectional, pull-up */

  esp_configgpio(BOARD_SDMMC_SLOT1_D0, OUTPUT_FUNCTION_1 | INPUT_FUNCTION_1 |
                                        PULLUP);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_D0,
                       SD_CARD_CDATA0_2_PAD_OUT_IDX, false, false);
  esp_gpio_matrix_in(BOARD_SDMMC_SLOT1_D0,
                      SD_CARD_CDATA0_2_PAD_IN_IDX, false);

  /* D1: bidirectional, pull-up */

  esp_configgpio(BOARD_SDMMC_SLOT1_D1, OUTPUT_FUNCTION_1 | INPUT_FUNCTION_1 |
                                        PULLUP);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_D1,
                       SD_CARD_CDATA1_2_PAD_OUT_IDX, false, false);
  esp_gpio_matrix_in(BOARD_SDMMC_SLOT1_D1,
                      SD_CARD_CDATA1_2_PAD_IN_IDX, false);

  /* D2: bidirectional, pull-up */

  esp_configgpio(BOARD_SDMMC_SLOT1_D2, OUTPUT_FUNCTION_1 | INPUT_FUNCTION_1 |
                                        PULLUP);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_D2,
                       SD_CARD_CDATA2_2_PAD_OUT_IDX, false, false);
  esp_gpio_matrix_in(BOARD_SDMMC_SLOT1_D2,
                      SD_CARD_CDATA2_2_PAD_IN_IDX, false);

  /* D3: bidirectional, pull-up */

  esp_configgpio(BOARD_SDMMC_SLOT1_D3, OUTPUT_FUNCTION_1 | INPUT_FUNCTION_1 |
                                        PULLUP);
  esp_gpio_matrix_out(BOARD_SDMMC_SLOT1_D3,
                       SD_CARD_CDATA3_2_PAD_OUT_IDX, false, false);
  esp_gpio_matrix_in(BOARD_SDMMC_SLOT1_D3,
                      SD_CARD_CDATA3_2_PAD_IN_IDX, false);

  syslog(LOG_INFO, "SDMMC slot1 GPIO matrix pins configured: "
         "CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d\n",
         BOARD_SDMMC_SLOT1_CLK, BOARD_SDMMC_SLOT1_CMD,
         BOARD_SDMMC_SLOT1_D0,  BOARD_SDMMC_SLOT1_D1,
         BOARD_SDMMC_SLOT1_D2,  BOARD_SDMMC_SLOT1_D3);
}
#endif /* CONFIG_ESP32P4_SDMMC_SLOT1 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_sdmmc_initialize
 *
 * Description:
 *   Initialize SDMMC subsystem for the ESP32-P4 Function EV Board.
 *
 *   Slot 0 (TF card): IOMUX pins configured by chip driver.
 *   Slot 1 (ESP-Hosted): GPIO matrix pins configured here before calling
 *   sdio_initialize(), then registered as /dev/mmcsd1.
 *
 ****************************************************************************/

int board_sdmmc_initialize(void)
{
  struct sdio_dev_s *sdio;
  int ret = OK;

#ifdef CONFIG_ESP32P4_SDMMC_SLOT0
  /* Slot 0: TF card (IOMUX, chip driver configures pins) */

  ret = esp32p4_sdmmc_slot0_enable_power();
  if (ret < 0)
    {
      return ret;
    }

  sdio = sdio_initialize(0);
  if (sdio == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SDIO slot 0\n");
      return -ENODEV;
    }

#if defined(CONFIG_MMCSD) && defined(CONFIG_MMCSD_SDIO)
  ret = mmcsd_slotinitialize(0, sdio);
  if (ret != OK)
    {
      syslog(LOG_ERR, "ERROR: Failed to register MMC/SD slot 0: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "SDMMC slot 0 registered as /dev/mmcsd0\n");
    }
#endif /* defined(CONFIG_MMCSD) && defined(CONFIG_MMCSD_SDIO) */
#endif /* CONFIG_ESP32P4_SDMMC_SLOT0 */

#ifdef CONFIG_ESP32P4_SDMMC_SLOT1
  /* Slot 1: ESP-Hosted SDIO to ESP32-C6 (GPIO matrix).
   *
   * Configure GPIO matrix pins BEFORE sdio_initialize(1), because the chip
   * driver's sdio_initialize() does NOT configure pins for slot 1 (it only
   * handles slot 0 IOMUX). After sdio_initialize() returns, the SDMMC
   * controller uses whatever GPIO matrix routing we set up here.
   */

  esp32p4_sdmmc_slot1_configure_pins();

  sdio = sdio_initialize(1);
  if (sdio == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SDIO slot 1\n");
      return -ENODEV;
    }

#if defined(CONFIG_MMCSD) && defined(CONFIG_MMCSD_SDIO)
  ret = mmcsd_slotinitialize(1, sdio);
  if (ret != OK)
    {
      syslog(LOG_ERR, "ERROR: Failed to register MMC/SD slot 1: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "SDMMC slot 1 registered as /dev/mmcsd1\n");
    }
#endif /* defined(CONFIG_MMCSD) && defined(CONFIG_MMCSD_SDIO) */
#endif /* CONFIG_ESP32P4_SDMMC_SLOT1 */

  return ret;
}
