/****************************************************************************
 * vendor/espressif/chips/esp32p4/espressif/esp_hosted/esp_netif.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version
 * 2.0 (the "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

#ifndef __ESP_NETIF_H__
#define __ESP_NETIF_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include "esp_err.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Opaque type for esp_netif - NuttX does not use esp_netif */

typedef struct esp_netif_t esp_netif_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: esp_netif_get_handle_from_ifkey
 *
 * Description:
 *   Stub for NuttX. Always returns NULL since NuttX does not use esp_netif.
 *
 ****************************************************************************/

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key);

/****************************************************************************
 * Name: esp_netif_is_netif_up
 *
 * Description:
 *   Stub for NuttX. Always returns false.
 *
 ****************************************************************************/

bool esp_netif_is_netif_up(esp_netif_t *esp_netif);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_NETIF_H__ */
