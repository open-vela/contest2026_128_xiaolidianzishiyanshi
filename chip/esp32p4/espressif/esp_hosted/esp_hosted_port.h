/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_hosted_port.h
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

#ifndef __ESP_HOSTED_PORT_H__
#define __ESP_HOSTED_PORT_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_event.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Hosted configuration macros */

#define HOSTED_BLOCK_MAX    -1
#define HOSTED_BLOCK_0       0

#define HOSTED_FREE(ptr) do { \
    if (ptr) { \
        g_h.funcs->_h_free(ptr); \
        (ptr) = NULL; \
    } \
} while(0)

/* Task priority and stack size defaults */

#define DFLT_TASK_PRIO          CONFIG_ESP_HOSTED_TASK_PRIORITY
#define DFLT_TASK_STACK_SIZE    CONFIG_ESP_HOSTED_TASK_STACK_SIZE
#define RPC_TASK_STACK_SIZE     CONFIG_ESP_HOSTED_TASK_STACK_SIZE

/* Log tag definition */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_port_init
 *
 * Description:
 *   Initialize the ESP-Hosted port layer. Populates g_hosted_osi_funcs
 *   with NuttX implementations.
 *
 ****************************************************************************/

int esp_hosted_port_init(void);

#endif /* __ESP_HOSTED_PORT_H__ */
