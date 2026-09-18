/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_board_esp_hosted.c
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

#include <sys/types.h>
#include <syslog.h>

#include "espressif/esp_hosted/esp_hosted_port.h"
#ifdef CONFIG_ESP_HOSTED_NETDEV
#  include "espressif/esp_hosted/esp_hosted_netdev.h"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_esp_hosted_initialize
 *
 * Description:
 *   Initialize the ESP-Hosted subsystem on the ESP32-P4 Function EV Board.
 *
 *   ESP-Hosted-MCU provides wireless connectivity by using the onboard
 *   ESP32-C6 as a WiFi/BLE co-processor via SDIO transport.
 *
 *   The ESP-Hosted transport owns SDMMC slot 1.  The normal MMC/SD block
 *   driver must not register that slot before this function is called.
 *
 ****************************************************************************/

int board_esp_hosted_initialize(void)
{
  int ret;

  /* Initialize ESP-Hosted port layer (timer signal handler, etc.) */

  ret = esp_hosted_port_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_hosted_port_init failed: %d\n", ret);
      return ret;
    }

  ret = esp_hosted_port_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_hosted_port_start failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_ESP_HOSTED_NETDEV
  ret = esp_hosted_netdev_register();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_hosted_netdev_register failed: %d\n", ret);
      return ret;
    }
#endif

  syslog(LOG_INFO, "ESP-Hosted SDIO transport and RPC initialized\n");
  return OK;
}
