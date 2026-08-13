/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_spi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port SPI interface for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_SPI_H_
#define __PORT_ESP_HOSTED_HOST_SPI_H_

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_SPI_BUFFER_SIZE  1600
#define MAX_TRANSPORT_BUFFER_SIZE  MAX_SPI_BUFFER_SIZE

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void *hosted_spi_init(void);
int hosted_spi_deinit(void *handle);
int hosted_do_spi_transfer(void *trans);

#endif /* __PORT_ESP_HOSTED_HOST_SPI_H_ */
