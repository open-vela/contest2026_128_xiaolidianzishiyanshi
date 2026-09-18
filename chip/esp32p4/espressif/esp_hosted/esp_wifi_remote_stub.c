/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_wifi_remote_stub.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>

#include <nuttx/kmalloc.h>

#include "esp_wifi_remote.h"
#ifdef CONFIG_ESP_HOSTED_NETDEV
#  include "esp_hosted_netdev.h"
#endif

typedef esp_err_t (*hosted_channel_tx_t)(void *h, void *buffer, size_t len);

static esp_remote_channel_t g_channels[WIFI_IF_MAX];
static hosted_channel_tx_t g_channel_tx[WIFI_IF_MAX];

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_wifi_remote_channel_rx
 *
 * Description:
 *   Consume a frame received on an ESP-Hosted Wi-Fi data channel.  The
 *   NuttX network lower-half is not connected yet, so frames are dropped,
 *   but ownership of the transport allocation is still honored.
 *
 ****************************************************************************/

esp_err_t esp_wifi_remote_channel_rx(void *h, void *buffer,
                                     void *buff_to_free, size_t len)
{
  esp_err_t ret = ESP_OK;

#ifdef CONFIG_ESP_HOSTED_NETDEV
  if (h == g_channels[WIFI_IF_STA])
    {
      ret = esp_hosted_netdev_rx_notify(buffer, len);
    }
#else
  (void)h;
  (void)buffer;
  (void)len;
#endif

  if (buff_to_free != NULL)
    {
      kumm_free(buff_to_free);
    }

  return ret;
}

/****************************************************************************
 * Name: esp_wifi_remote_channel_set
 *
 * Description:
 *   Wi-Fi Remote normally records these channel callbacks in its ESP-IDF
 *   network adapter.  Transport and RPC operation do not require that
 *   adapter, so retain this as a no-op until the NuttX netdev is attached.
 *
 ****************************************************************************/

void esp_wifi_remote_channel_set(int wifi_if,
                                 esp_remote_channel_t channel,
                                 void *tx_fn)
{
  if (wifi_if >= 0 && wifi_if < WIFI_IF_MAX)
    {
      g_channels[wifi_if] = channel;
      g_channel_tx[wifi_if] = (hosted_channel_tx_t)tx_fn;
    }
}

esp_err_t esp_wifi_remote_channel_tx(int wifi_if, void *buffer, size_t len)
{
  if (wifi_if < 0 || wifi_if >= WIFI_IF_MAX ||
      g_channels[wifi_if] == NULL || g_channel_tx[wifi_if] == NULL)
    {
      return ESP_ERR_INVALID_STATE;
    }

  return g_channel_tx[wifi_if](g_channels[wifi_if], buffer, len);
}
