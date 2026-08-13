/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_spi_hd.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port SPI-HD interface for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_SPI_HD_H_
#define __PORT_ESP_HOSTED_HOST_SPI_HD_H_

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_SPI_HD_BUFFER_SIZE  1600
#define MAX_TRANSPORT_BUFFER_SIZE  MAX_SPI_HD_BUFFER_SIZE

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void *hosted_spi_hd_init(void);
esp_err_t hosted_spi_hd_deinit(void *ctx);

int hosted_spi_hd_read_reg(void *ctx, uint32_t reg, uint8_t *data,
                           uint16_t size, bool lock_required);
int hosted_spi_hd_write_reg(void *ctx, uint32_t reg, uint8_t *data,
                            uint16_t size, bool lock_required);
int hosted_spi_hd_read_block(void *ctx, uint32_t reg, uint8_t *data,
                             uint16_t size, bool lock_required);
int hosted_spi_hd_write_block(void *ctx, uint32_t reg, uint8_t *data,
                              uint16_t size, bool lock_required);
int hosted_spi_hd_wait_slave_intr(void *ctx, uint32_t ticks_to_wait);

#endif /* __PORT_ESP_HOSTED_HOST_SPI_HD_H_ */
