/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/
 * port_esp_hosted_host_sdio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_HOSTED_SDIO_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_HOSTED_SDIO_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#define ESP_HOSTED_SDIO_BLOCK_SIZE 512
#define ESP_HOSTED_SDIO_UNRESPONSIVE_CODE 0x107

#ifndef MAX_TRANSPORT_BUFFER_SIZE
#  define MAX_TRANSPORT_BUFFER_SIZE MAX_SDIO_BUFFER_SIZE
#endif

int esp_hosted_sdio_probe(void);

void *hosted_sdio_init(void);
int hosted_sdio_deinit(void *ctx);
int hosted_sdio_card_init(void *ctx, bool show_config);
int hosted_sdio_card_deinit(void *ctx);
int hosted_sdio_read_reg(void *ctx, uint32_t reg, uint8_t *data,
                         uint16_t size, bool lock_required);
int hosted_sdio_write_reg(void *ctx, uint32_t reg, uint8_t *data,
                          uint16_t size, bool lock_required);
int hosted_sdio_read_block(void *ctx, uint32_t reg, uint8_t *data,
                           uint16_t size, bool lock_required);
int hosted_sdio_write_block(void *ctx, uint32_t reg, uint8_t *data,
                            uint16_t size, bool lock_required);
int hosted_sdio_wait_slave_intr(void *ctx, uint32_t ticks_to_wait);

#endif
