/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_wifi_remote.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stub for ESP-IDF esp_wifi_remote.h on NuttX.
 * Provides wifi type definitions and remote channel types needed by
 * esp-hosted API layer.
 *
 ****************************************************************************/

#ifndef __ESP_WIFI_REMOTE_H__
#define __ESP_WIFI_REMOTE_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Remote channel - forward declaration only, defined in esp_hosted_api.c */

struct esp_remote_channel;

/* Remote channel handle type */

typedef struct esp_remote_channel *esp_remote_channel_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_wifi_remote_channel_rx
 *
 * Description:
 *   Callback for receiving data on a remote WiFi channel.
 *
 ****************************************************************************/

esp_err_t esp_wifi_remote_channel_rx(void *h, void *buffer,
                                     void *buff_to_free, size_t len);

/****************************************************************************
 * Name: esp_wifi_remote_channel_set
 *
 * Description:
 *   Set the remote channel for a WiFi interface.
 *
 ****************************************************************************/

void esp_wifi_remote_channel_set(int wifi_if,
                                 esp_remote_channel_t channel,
                                 void *tx_fn);

/****************************************************************************
 * Name: esp_wifi_remote_connect
 *
 * Description:
 *   Connect to remote WiFi.
 *
 ****************************************************************************/

esp_err_t esp_wifi_remote_connect(void);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_WIFI_REMOTE_H__ */
