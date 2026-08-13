/****************************************************************************
 * vendor/espressif/chips/esp32p4/espressif/esp_hosted/esp_netif_stub.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "esp_netif.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_netif_get_handle_from_ifkey
 *
 * Description:
 *   NuttX stub - NuttX does not use esp_netif for network interface
 *   management. Always returns NULL.
 *
 ****************************************************************************/

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key)
{
  (void)if_key;
  return (esp_netif_t *)0;
}

/****************************************************************************
 * Name: esp_netif_is_netif_up
 *
 * Description:
 *   NuttX stub - NuttX manages network interfaces differently.
 *   Always returns false.
 *
 ****************************************************************************/

bool esp_netif_is_netif_up(esp_netif_t *esp_netif)
{
  (void)esp_netif;
  return false;
}
