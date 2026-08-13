/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port configuration for ESP-Hosted.
 * Replaces the upstream ESP-IDF FreeRTOS port config header.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_CONFIG_H__
#define __PORT_ESP_HOSTED_HOST_CONFIG_H__

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include "esp_netif.h"

/****************************************************************************
 * Transport Type Constants
 ****************************************************************************/

#define H_TRANSPORT_NONE    0
#define H_TRANSPORT_SDIO    1
#define H_TRANSPORT_SPI_HD  2
#define H_TRANSPORT_SPI     3
#define H_TRANSPORT_UART    4

/****************************************************************************
 * Transport Selection
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SDIO
#  define H_TRANSPORT_IN_USE H_TRANSPORT_SDIO
#elif defined(CONFIG_ESP_HOSTED_SPI_HD)
#  define H_TRANSPORT_IN_USE H_TRANSPORT_SPI_HD
#elif defined(CONFIG_ESP_HOSTED_SPI)
#  define H_TRANSPORT_IN_USE H_TRANSPORT_SPI
#elif defined(CONFIG_ESP_HOSTED_UART)
#  define H_TRANSPORT_IN_USE H_TRANSPORT_UART
#else
#  define H_TRANSPORT_IN_USE H_TRANSPORT_NONE
#endif

/****************************************************************************
 * GPIO Definitions
 ****************************************************************************/

#define H_GPIO_LOW   0
#define H_GPIO_HIGH  1
#define H_ENABLE     1
#define H_DISABLE    0

enum
{
  H_GPIO_INTR_DISABLE = 0,
  H_GPIO_INTR_POSEDGE = 1,
  H_GPIO_INTR_NEGEDGE = 2,
  H_GPIO_INTR_ANYEDGE = 3,
  H_GPIO_INTR_LOW_LEVEL = 4,
  H_GPIO_INTR_HIGH_LEVEL = 5,
  H_GPIO_INTR_MAX,
};

/****************************************************************************
 * Slave Target Selection
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32
#  define H_SLAVE_TARGET_ESP32 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32S2
#  define H_SLAVE_TARGET_ESP32S2 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32C3
#  define H_SLAVE_TARGET_ESP32C3 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32S3
#  define H_SLAVE_TARGET_ESP32S3 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32C2
#  define H_SLAVE_TARGET_ESP32C2 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6
#  define H_SLAVE_TARGET_ESP32C6 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
#  define H_SLAVE_TARGET_ESP32C5 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32C61
#  define H_SLAVE_TARGET_ESP32C61 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32H2
#  define H_SLAVE_TARGET_ESP32H2 1
#endif
#ifdef CONFIG_ESP_HOSTED_CP_TARGET_ESP32H4
#  define H_SLAVE_TARGET_ESP32H4 1
#endif

/****************************************************************************
 * Mempool Configuration
 ****************************************************************************/

/* NuttX uses its own heap management, disable upstream mempool */

#define H_USE_MEMPOOL 0

/****************************************************************************
 * RPC Configuration
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_MAX_SYNC_RPC_REQUESTS
#  define H_MAX_SYNC_RPC_REQUESTS CONFIG_ESP_HOSTED_MAX_SYNC_RPC_REQUESTS
#else
#  define H_MAX_SYNC_RPC_REQUESTS 5
#endif

#ifdef CONFIG_ESP_HOSTED_MAX_ASYNC_RPC_REQUESTS
#  define H_MAX_ASYNC_RPC_REQUESTS CONFIG_ESP_HOSTED_MAX_ASYNC_RPC_REQUESTS
#else
#  define H_MAX_ASYNC_RPC_REQUESTS 5
#endif

/****************************************************************************
 * SDIO Transport Configuration
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SDIO

#  define H_SDIO_CLOCK_FREQ_KHZ  CONFIG_ESP_HOSTED_SDIO_FREQ_KHZ
#  define H_SDIO_BUS_WIDTH       CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH_4BIT ? 4 : 1
#  define H_SDMMC_HOST_SLOT      CONFIG_ESP_HOSTED_SDIO_SLOT

#  define H_SDIO_PORT_CLK  NULL
#  define H_SDIO_PORT_CMD  NULL
#  define H_SDIO_PORT_D0   NULL
#  define H_SDIO_PORT_D1   NULL
#  define H_SDIO_PORT_D2   NULL
#  define H_SDIO_PORT_D3   NULL

#  define H_SDIO_PIN_CLK  -1
#  define H_SDIO_PIN_CMD  -1
#  define H_SDIO_PIN_D0   -1
#  define H_SDIO_PIN_D1   -1
#  define H_SDIO_PIN_D2   -1
#  define H_SDIO_PIN_D3   -1

#  define H_SDIO_TX_Q  CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE
#  define H_SDIO_RX_Q  CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE

#  define H_SDIO_CHECKSUM  CONFIG_ESP_HOSTED_SDIO_CHECKSUM

#  define H_SDIO_HOST_STREAMING_MODE 1
#  define H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE 2
#  define H_SDIO_OPTIMIZATION_RX_NONE 3

#  define H_SDIO_HOST_RX_MODE H_SDIO_OPTIMIZATION_RX_NONE

#  define H_SDIO_TX_LEN_TO_TRANSFER(x) (((x) + 3) & (~3))
#  define H_SDIO_RX_LEN_TO_TRANSFER(x) (((x) + 3) & (~3))

#  define H_SDIO_TX_BLOCK_ONLY_XFER  1
#  define H_SDIO_RX_BLOCK_ONLY_XFER  1

#  define H_SDIO_TX_BLOCKS_TO_TRANSFER(x) ((x) / ESP_BLOCK_SIZE)
#  define H_SDIO_RX_BLOCKS_TO_TRANSFER(x) ((x) / ESP_BLOCK_SIZE)

#  define H_TRANSPORT_QUEUE_SIZE  CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE

#endif /* CONFIG_ESP_HOSTED_SDIO */

/****************************************************************************
 * SPI Transport Configuration
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE

#  ifdef CONFIG_ESP_HOSTED_HS_ACTIVE_LOW
#    define H_HANDSHAKE_ACTIVE_HIGH 0
#  else
#    define H_HANDSHAKE_ACTIVE_HIGH 1
#  endif

#  ifdef CONFIG_ESP_HOSTED_DR_ACTIVE_LOW
#    define H_DATAREADY_ACTIVE_HIGH 0
#  else
#    define H_DATAREADY_ACTIVE_HIGH 1
#  endif

#  if H_HANDSHAKE_ACTIVE_HIGH
#    define H_HS_VAL_ACTIVE    H_GPIO_HIGH
#    define H_HS_VAL_INACTIVE  H_GPIO_LOW
#    define H_HS_INTR_EDGE     H_GPIO_INTR_POSEDGE
#  else
#    define H_HS_VAL_ACTIVE    H_GPIO_LOW
#    define H_HS_VAL_INACTIVE  H_GPIO_HIGH
#    define H_HS_INTR_EDGE     H_GPIO_INTR_NEGEDGE
#  endif

#  if H_DATAREADY_ACTIVE_HIGH
#    define H_DR_VAL_ACTIVE    H_GPIO_HIGH
#    define H_DR_VAL_INACTIVE  H_GPIO_LOW
#    define H_DR_INTR_EDGE     H_GPIO_INTR_POSEDGE
#  else
#    define H_DR_VAL_ACTIVE    H_GPIO_LOW
#    define H_DR_VAL_INACTIVE  H_GPIO_HIGH
#    define H_DR_INTR_EDGE     H_GPIO_INTR_NEGEDGE
#  endif

#  define H_GPIO_HANDSHAKE_Port   NULL
#  define H_GPIO_HANDSHAKE_Pin    CONFIG_ESP_HOSTED_SPI_GPIO_HANDSHAKE
#  define H_GPIO_DATA_READY_Port  NULL
#  define H_GPIO_DATA_READY_Pin   CONFIG_ESP_HOSTED_SPI_GPIO_DATA_READY

#  define H_GPIO_MOSI_Port  NULL
#  define H_GPIO_MOSI_Pin   CONFIG_ESP_HOSTED_SPI_GPIO_MOSI
#  define H_GPIO_MISO_Port  NULL
#  define H_GPIO_MISO_Pin   CONFIG_ESP_HOSTED_SPI_GPIO_MISO
#  define H_GPIO_SCLK_Port  NULL
#  define H_GPIO_SCLK_Pin   CONFIG_ESP_HOSTED_SPI_GPIO_CLK
#  define H_GPIO_CS_Port    NULL
#  define H_GPIO_CS_Pin     CONFIG_ESP_HOSTED_SPI_GPIO_CS

#  define H_SPI_TX_Q  CONFIG_ESP_HOSTED_SPI_TX_Q_SIZE
#  define H_SPI_RX_Q  CONFIG_ESP_HOSTED_SPI_RX_Q_SIZE

#  define H_SPI_MODE        CONFIG_ESP_HOSTED_SPI_MODE
#  define H_SPI_FD_CLK_MHZ  CONFIG_ESP_HOSTED_SPI_CLK_FREQ

#  define H_TRANSPORT_QUEUE_SIZE  CONFIG_ESP_HOSTED_SPI_TX_Q_SIZE

#endif /* CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE */

/****************************************************************************
 * SPI-HD Transport Configuration
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE

#  define H_SPI_HD_HOST_INTERFACE 1

enum
{
  H_SPI_HD_CONFIG_1_DATA_LINE,
  H_SPI_HD_CONFIG_2_DATA_LINES,
  H_SPI_HD_CONFIG_4_DATA_LINES,
};

#  if CONFIG_ESP_HOSTED_SPI_HD_DR_ACTIVE_HIGH
#    define H_SPI_HD_DATAREADY_ACTIVE_HIGH 1
#  else
#    define H_SPI_HD_DATAREADY_ACTIVE_HIGH 0
#  endif

#  if H_SPI_HD_DATAREADY_ACTIVE_HIGH
#    define H_SPI_HD_DR_VAL_ACTIVE    H_GPIO_HIGH
#    define H_SPI_HD_DR_VAL_INACTIVE  H_GPIO_LOW
#    define H_SPI_HD_DR_INTR_EDGE     H_GPIO_INTR_POSEDGE
#  else
#    define H_SPI_HD_DR_VAL_ACTIVE    H_GPIO_LOW
#    define H_SPI_HD_DR_VAL_INACTIVE  H_GPIO_HIGH
#    define H_SPI_HD_DR_INTR_EDGE     H_GPIO_INTR_NEGEDGE
#  endif

#  define H_SPI_HD_HOST_NUM_DATA_LINES  CONFIG_ESP_HOSTED_SPI_HD_INTERFACE_NUM_DATA_LINES

#  define H_SPI_HD_PORT_D0   NULL
#  define H_SPI_HD_PORT_D1   NULL
#  define H_SPI_HD_PORT_D2   NULL
#  define H_SPI_HD_PORT_D3   NULL
#  define H_SPI_HD_PORT_CS   NULL
#  define H_SPI_HD_PORT_CLK  NULL

#  define H_SPI_HD_PIN_D0  CONFIG_ESP_HOSTED_SPI_HD_GPIO_D0
#  if (CONFIG_ESP_HOSTED_SPI_HD_INTERFACE_NUM_DATA_LINES >= 2)
#    define H_SPI_HD_PIN_D1  CONFIG_ESP_HOSTED_SPI_HD_GPIO_D1
#  else
#    define H_SPI_HD_PIN_D1  -1
#  endif
#  if (CONFIG_ESP_HOSTED_SPI_HD_INTERFACE_NUM_DATA_LINES == 4)
#    define H_SPI_HD_PIN_D2  CONFIG_ESP_HOSTED_SPI_HD_GPIO_D2
#    define H_SPI_HD_PIN_D3  CONFIG_ESP_HOSTED_SPI_HD_GPIO_D3
#  else
#    define H_SPI_HD_PIN_D2  -1
#    define H_SPI_HD_PIN_D3  -1
#  endif

#  define H_SPI_HD_PIN_CS   CONFIG_ESP_HOSTED_SPI_HD_GPIO_CS
#  define H_SPI_HD_PIN_CLK  CONFIG_ESP_HOSTED_SPI_HD_GPIO_CLK

#  define H_SPI_HD_PORT_DATA_READY  NULL
#  ifdef CONFIG_ESP_HOSTED_SPI_HD_DATA_READY_ENABLED
#    define H_SPI_HD_PIN_DATA_READY  CONFIG_ESP_HOSTED_SPI_HD_GPIO_DATA_READY
#    define H_SPI_HD_DATA_READY_ENABLED 1
#  else
#    define H_SPI_HD_PIN_DATA_READY  -1
#    define H_SPI_HD_DATA_READY_ENABLED 0
#    define H_SPI_HD_POLL_INTERVAL_MS  CONFIG_ESP_HOSTED_SPI_HD_POLL_INTERVAL_MS
#  endif

#  define H_SPI_HD_CLK_MHZ  CONFIG_ESP_HOSTED_SPI_HD_CLK_FREQ
#  define H_SPI_HD_MODE     CONFIG_ESP_HOSTED_SPI_HD_MODE

#  define H_SPI_HD_TX_QUEUE_SIZE  CONFIG_ESP_HOSTED_SPI_HD_TX_Q_SIZE
#  define H_SPI_HD_RX_QUEUE_SIZE  CONFIG_ESP_HOSTED_SPI_HD_RX_Q_SIZE

#  define H_SPI_HD_CHECKSUM  CONFIG_ESP_HOSTED_SPI_HD_CHECKSUM

#  define H_SPI_HD_NUM_COMMAND_BITS  8
#  define H_SPI_HD_NUM_ADDRESS_BITS  8
#  define H_SPI_HD_NUM_DUMMY_BITS    8

#  define H_TRANSPORT_QUEUE_SIZE  CONFIG_ESP_HOSTED_SPI_HD_TX_Q_SIZE

#else
#  define H_SPI_HD_HOST_INTERFACE 0
#endif /* CONFIG_ESP_HOSTED_SPI_HD_HOST_INTERFACE */

/****************************************************************************
 * UART Transport Configuration
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_UART_HOST_INTERFACE
#  define H_UART_HOST_TRANSPORT 1
#else
#  define H_UART_HOST_TRANSPORT 0
#endif

/****************************************************************************
 * Reset Pin Configuration
 ****************************************************************************/

#define H_GPIO_PIN_RESET   CONFIG_ESP_HOSTED_GPIO_RESET_SLAVE
#define H_GPIO_PORT_RESET  NULL

#ifdef CONFIG_ESP_HOSTED_RESET_GPIO_ACTIVE_LOW
#  define H_RESET_ACTIVE_HIGH 0
#else
#  define H_RESET_ACTIVE_HIGH 1
#endif

#if H_RESET_ACTIVE_HIGH
#  define H_RESET_VAL_ACTIVE    H_GPIO_HIGH
#  define H_RESET_VAL_INACTIVE  H_GPIO_LOW
#else
#  define H_RESET_VAL_ACTIVE    H_GPIO_LOW
#  define H_RESET_VAL_INACTIVE  H_GPIO_HIGH
#endif

/****************************************************************************
 * Slave Reset Strategy
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP
#  define H_SLAVE_RESET_ON_EVERY_HOST_BOOTUP 1
#  define H_SLAVE_RESET_ONLY_IF_NECESSARY 0
#elif defined(CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY)
#  define H_SLAVE_RESET_ON_EVERY_HOST_BOOTUP 0
#  define H_SLAVE_RESET_ONLY_IF_NECESSARY 1
#else
#  define H_SLAVE_RESET_ON_EVERY_HOST_BOOTUP 1
#  define H_SLAVE_RESET_ONLY_IF_NECESSARY 0
#endif

/****************************************************************************
 * Transport Restart on Failure
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE
#  define H_TRANSPORT_RESTART_ON_FAILURE 1
#else
#  define H_TRANSPORT_RESTART_ON_FAILURE 0
#endif

/****************************************************************************
 * Host Auto-Restart on Communication Failure
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE
#  define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE 1
#else
#  define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE 0
#endif

#ifdef CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT
#  define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS \
    (CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT * 1000)
#else
#  define H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS -1
#endif

/****************************************************************************
 * Serial Response Timeout
 ****************************************************************************/

#define TIMEOUT_PSERIAL_RESP  30

/****************************************************************************
 * Format Strings
 ****************************************************************************/

#define PRE_FORMAT_NEWLINE_CHAR   ""
#define POST_FORMAT_NEWLINE_CHAR  "\n"

/****************************************************************************
 * Memory Allocation
 ****************************************************************************/

#define USE_STD_C_LIB_MALLOC  0

/****************************************************************************
 * Weak Reference Attribute
 ****************************************************************************/

#define H_WEAK_REF  __attribute__((weak))

/****************************************************************************
 * Raw Throughput Testing
 ****************************************************************************/

#define H_TEST_RAW_TP  CONFIG_ESP_HOSTED_RAW_THROUGHPUT_TRANSPORT

#if H_TEST_RAW_TP
#  define H_RAW_TP_REPORT_INTERVAL  CONFIG_ESP_HOSTED_RAW_TP_REPORT_INTERVAL
#  define H_RAW_TP_PKT_LEN          CONFIG_ESP_HOSTED_RAW_TP_HOST_TO_ESP_PKT_LEN
#  if CONFIG_ESP_HOSTED_RAW_THROUGHPUT_TX_TO_SLAVE
#    define H_TEST_RAW_TP_DIR  1 /* HOST_TO_ESP */
#  elif CONFIG_ESP_HOSTED_RAW_THROUGHPUT_RX_FROM_SLAVE
#    define H_TEST_RAW_TP_DIR  2 /* ESP_TO_HOST */
#  elif CONFIG_ESP_HOSTED_RAW_THROUGHPUT_BIDIRECTIONAL
#    define H_TEST_RAW_TP_DIR  3 /* BIDIRECTIONAL */
#  else
#    define H_TEST_RAW_TP_DIR  0 /* NONE */
#  endif
#else
#  define H_TEST_RAW_TP_DIR  0 /* NONE */
#endif

/****************************************************************************
 * Memory Monitor
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
#  define H_MEM_MONITOR 1
#else
#  define H_MEM_MONITOR 0
#endif

/****************************************************************************
 * Packet Stats
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_PKT_STATS
#  define ESP_PKT_STATS 1
#  define ESP_PKT_STATS_REPORT_INTERVAL  CONFIG_ESP_HOSTED_PKT_STATS_INTERVAL_SEC
#endif

/****************************************************************************
 * Event Group Bit for WiFi Flow Control
 ****************************************************************************/

#define H_EVTGRP_BIT_FC_ALLOW_WIFI  (1 << 0)

/****************************************************************************
 * Host Power Save
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_HOST_POWER_SAVE_ENABLED
#  ifdef CONFIG_ESP_HOSTED_HOST_DEEP_SLEEP_ALLOWED
#    define H_HOST_PS_ALLOWED 1
#    define H_HOST_PS_ALLOWED_LIGHT_SLEEP 0
#  else
#    define H_HOST_PS_ALLOWED 0
#  endif
#  define H_HOST_USE_HP_PERIPH_POWERDOWN 0
#else
#  define H_HOST_PS_ALLOWED 0
#endif

#ifdef CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO
#  define H_HOST_WAKEUP_GPIO_PORT  NULL
#  define H_HOST_WAKEUP_GPIO       CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO
#else
#  define H_HOST_WAKEUP_GPIO  -1
#endif

#ifdef CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO_LEVEL
#  define H_HOST_WAKEUP_GPIO_LEVEL  CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO_LEVEL
#else
#  define H_HOST_WAKEUP_GPIO_LEVEL  1
#endif

/****************************************************************************
 * SDIO Reset Delay
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS
#  define H_HOST_SDIO_RESET_DELAY_MS  CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS
#else
#  define H_HOST_SDIO_RESET_DELAY_MS  1500
#endif

/****************************************************************************
 * CLI
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_CLI_ENABLED
#  define H_ESP_HOSTED_CLI_ENABLED 1
#endif

/****************************************************************************
 * Peer Data Transfer
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#  define H_PEER_DATA_TRANSFER 1
#  ifdef CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS
#    define H_MAX_CUSTOM_MSG_HANDLERS  CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS
#  endif
#else
#  define H_PEER_DATA_TRANSFER 0
#endif

/****************************************************************************
 * Network Split
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
#  define H_NETWORK_SPLIT_ENABLED 1
#else
#  define H_NETWORK_SPLIT_ENABLED 0
#endif

/****************************************************************************
 * GPIO Expander
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER
#  define H_GPIO_EXPANDER_SUPPORT 1
#else
#  define H_GPIO_EXPANDER_SUPPORT 0
#endif

/****************************************************************************
 * External Coexistence
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_CP_EXT_COEX
#  define H_EXT_COEX_SUPPORT 1
#else
#  define H_EXT_COEX_SUPPORT 0
#endif

#ifdef CONFIG_ESP_HOSTED_CP_EXT_COEX_ADVANCE
#  define H_EXT_COEX_ADVANCE_SUPPORT 1
#else
#  define H_EXT_COEX_ADVANCE_SUPPORT 0
#endif

/****************************************************************************
 * Static Netif
 ****************************************************************************/

#define H_HOST_USES_STATIC_NETIF  0

/****************************************************************************
 * ESP-Netif error codes
 ****************************************************************************/

#ifndef ESP_ERR_ESP_NETIF_NO_MEM
#  define ESP_ERR_ESP_NETIF_NO_MEM          0x5006
#endif

#ifndef ESP_ERR_ESP_NETIF_INVALID_PARAMS
#  define ESP_ERR_ESP_NETIF_INVALID_PARAMS  0x5002
#endif

/****************************************************************************
 * WiFi Max Connection
 ****************************************************************************/

#ifndef ESP_WIFI_MAX_CONN_NUM
#  define ESP_WIFI_MAX_CONN_NUM  (15)
#endif

/****************************************************************************
 * WiFi TX Data Throttle
 ****************************************************************************/

#ifdef CONFIG_HOST_TO_ESP_WIFI_DATA_THROTTLE
#  define H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD \
    CONFIG_ESP_HOSTED_TO_WIFI_DATA_THROTTLE_LOW_THRESHOLD
#  define H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD \
    CONFIG_ESP_HOSTED_TO_WIFI_DATA_THROTTLE_HIGH_THRESHOLD
#else
#  define H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD  0
#  define H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD 0
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

esp_err_t esp_hosted_set_default_config(void);
bool esp_hosted_is_config_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* __PORT_ESP_HOSTED_HOST_CONFIG_H__ */
