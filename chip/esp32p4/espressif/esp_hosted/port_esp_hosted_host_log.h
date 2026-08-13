/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_log.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port log configuration for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_LOG_H
#define __PORT_ESP_HOSTED_HOST_LOG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "esp_log.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef DEFINE_LOG_TAG
#define DEFINE_LOG_TAG(sTr) static const char TAG[] = #sTr
#endif

#endif /* __PORT_ESP_HOSTED_HOST_LOG_H */
