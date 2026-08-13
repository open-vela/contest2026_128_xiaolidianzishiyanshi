/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_openthread.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port OpenThread configuration for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_OPENTHREAD_H_
#define __PORT_ESP_HOSTED_HOST_OPENTHREAD_H_

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * OpenThread Feature Flags
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_HOST_OT_ENABLE
#  define H_HOST_OT_ENABLE 1
#else
#  define H_HOST_OT_ENABLE 0
#endif

#ifdef CONFIG_ESP_HOSTED_OT_TRANSPORT_UART
#  define H_OT_TRANSPORT_UART_DEDICATED 1
#else
#  define H_OT_TRANSPORT_UART_DEDICATED 0
#endif

#ifdef CONFIG_ESP_HOSTED_OT_TRANSPORT_HOSTED
#  define H_OT_TRANSPORT_HOSTED 1
#else
#  define H_OT_TRANSPORT_HOSTED 0
#endif

#endif /* __PORT_ESP_HOSTED_HOST_OPENTHREAD_H_ */
