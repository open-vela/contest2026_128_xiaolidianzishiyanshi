/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_INTERNAL_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_INTERNAL_H

#include "esp32p4_ppa.h"

#include "soc/ppa_struct.h"

#define ESP32P4_PPA_CLIENT_MAGIC 0x50504143u

struct esp32p4_ppa_client_s
{
  uint32_t magic;
  enum esp32p4_ppa_operation_e operation;
  enum esp32p4_ppa_burst_length_e burst_length;
};

int esp32p4_ppa_hw_acquire(void);
int esp32p4_ppa_hw_release(void);
int esp32p4_ppa_hal_fill(esp32p4_ppa_handle_t handle,
                         const struct esp32p4_ppa_fill_config_s *config);
int esp32p4_ppa_hal_blend(esp32p4_ppa_handle_t handle,
                          const struct esp32p4_ppa_blend_config_s *config);
int esp32p4_ppa_hal_srm(esp32p4_ppa_handle_t handle,
                        const struct esp32p4_ppa_srm_config_s *config);
void esp32p4_ppa_hal_configure_fill(
  ppa_dev_t *dev, const struct esp32p4_ppa_fill_config_s *config,
  uint32_t *configured_color);
void esp32p4_ppa_hal_configure_blend(
  ppa_dev_t *dev, const struct esp32p4_ppa_blend_config_s *config);
void esp32p4_ppa_hal_configure_srm(
  ppa_dev_t *dev, const struct esp32p4_ppa_srm_config_s *config);
void esp32p4_ppa_hal_start_fill(ppa_dev_t *dev);
void esp32p4_ppa_hal_start_blend(ppa_dev_t *dev);
void esp32p4_ppa_hal_start_srm(ppa_dev_t *dev);

int esp32p4_ppa_dma2d_fill(esp32p4_ppa_handle_t handle,
                           const struct esp32p4_ppa_fill_config_s *config);
int esp32p4_ppa_dma2d_blend(esp32p4_ppa_handle_t handle,
                            const struct esp32p4_ppa_blend_config_s *config);
int esp32p4_ppa_dma2d_srm(esp32p4_ppa_handle_t handle,
                          const struct esp32p4_ppa_srm_config_s *config);

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_INTERNAL_H */
