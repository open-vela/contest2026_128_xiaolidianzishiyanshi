/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_hosted_netdev.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __ESP_HOSTED_NETDEV_H__
#define __ESP_HOSTED_NETDEV_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/net/netdev_lowerhalf.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_netdev_register
 *
 * Description:
 *   Register the ESP-Hosted network device.
 *
 ****************************************************************************/

int esp_hosted_netdev_register(void);

/****************************************************************************
 * Name: esp_hosted_netdev_rx_notify
 *
 * Description:
 *   Notify the network device that new data is available from the
 *   ESP-Hosted transport layer. Called from the transport RX callback.
 *
 ****************************************************************************/

int esp_hosted_netdev_rx_notify(uint8_t *data, uint16_t len);

#endif /* __ESP_HOSTED_NETDEV_H__ */
