/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_uart.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port UART interface for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_UART_H_
#define __PORT_ESP_HOSTED_HOST_UART_H_

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include "esp_err.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_UART_BUFFER_SIZE  1600
#define MAX_TRANSPORT_BUFFER_SIZE  MAX_UART_BUFFER_SIZE

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void *hosted_uart_init(void);
esp_err_t hosted_uart_deinit(void *ctx);
int hosted_uart_read(void *ctx, uint8_t *data, uint16_t size);
int hosted_uart_write(void *ctx, uint8_t *data, uint16_t size);
int hosted_uart_flush_input(void *ctx);
int hosted_wait_rx_data(uint32_t ticks_to_wait);

#endif /* __PORT_ESP_HOSTED_HOST_UART_H_ */
