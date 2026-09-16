/****************************************************************************
 * arch/risc-v/src/esp32p4/espressif/esp_mipi_dsi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESPRESSIF_ESP_MIPI_DSI_H
#define __ARCH_RISCV_SRC_ESP32P4_ESPRESSIF_ESP_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/video/mipi_dsi.h>

#ifdef CONFIG_ESPRESSIF_MIPI_DSI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board-supplied DSI bus and DPI parameters.  The ESP32-P4 host driver has
 * no panel defaults; reset/power, DCS initialization and display timing all
 * belong to board/panel code.
 */

struct esp_mipi_dsi_config_s
{
  uint8_t lanes;
  uint16_t lane_rate_mbps;

  uint16_t width;
  uint16_t height;
  uint16_t hsync;
  uint16_t hbp;
  uint16_t hfp;
  uint16_t vsync;
  uint16_t vbp;
  uint16_t vfp;
  uint8_t bpp;
  uint8_t format;
  uint8_t dpi_clock_mhz;

  bool use_test_pattern;

  FAR struct mipi_dsi_device *(*panel_initialize)
    (FAR struct mipi_dsi_host *host);
  void (*backlight)(bool on);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

int esp_mipi_dsi_set_config(
      FAR const struct esp_mipi_dsi_config_s *config);

int esp_mipi_dsi_dcs_read(FAR struct mipi_dsi_device *device, uint8_t cmd,
                          FAR uint8_t *buf, size_t len);

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_ESPRESSIF_MIPI_DSI */
#endif /* __ARCH_RISCV_SRC_ESP32P4_ESPRESSIF_ESP_MIPI_DSI_H */
