/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_bt_config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port Bluetooth configuration for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_BT_CONFIG_H__
#define __PORT_ESP_HOSTED_HOST_BT_CONFIG_H__

/****************************************************************************
 * Bluetooth Feature Flags
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#  define H_BT_HOST_ESP_NIMBLE 1
#else
#  define H_BT_HOST_ESP_NIMBLE 0
#endif

#ifdef CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI
#  define H_BT_USE_VHCI 1
#else
#  define H_BT_USE_VHCI 0
#endif

#ifdef CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
#  define H_BT_HOST_ESP_BLUEDROID 1
#else
#  define H_BT_HOST_ESP_BLUEDROID 0
#endif

#ifdef CONFIG_ESP_HOSTED_BLUEDROID_HCI_VHCI
#  define H_BT_BLUEDROID_USE_VHCI 1
#else
#  define H_BT_BLUEDROID_USE_VHCI 0
#endif

#define H_BT_ENABLE_LL_INIT 0

#endif /* __PORT_ESP_HOSTED_HOST_BT_CONFIG_H__ */
