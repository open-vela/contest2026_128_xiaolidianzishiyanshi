/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-pico-wifi-wareshare/src/
 * esp32p4_board_esp_hosted.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>

#include "espressif/esp_hosted/esp_hosted_port.h"
#ifdef CONFIG_ESP_HOSTED_NETDEV
#  include "espressif/esp_hosted/esp_hosted_netdev.h"
#endif

int board_esp_hosted_initialize(void)
{
  int ret;

  ret = esp_hosted_port_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: OS port initialization failed: %d\n",
             ret);
      return ret;
    }

  ret = esp_hosted_port_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: transport/RPC startup failed: %d\n",
             ret);
      return ret;
    }

#ifdef CONFIG_ESP_HOSTED_NETDEV
  ret = esp_hosted_netdev_register();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: netdev registration failed: %d\n", ret);
      return ret;
    }
#endif

  syslog(LOG_INFO, "ESP-Hosted: SDIO transport and RPC initialized\n");
  return OK;
}
