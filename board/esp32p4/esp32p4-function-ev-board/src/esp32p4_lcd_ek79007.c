/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd_ek79007.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_mipi_dsi.h"

#include "esp32p4-function-ev-board.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EK79007_NAME "ek79007"
#define EK79007_LANES 2
#define EK79007_LANE_RATE_MBPS 650

#define EK79007_WIDTH 1024
#define EK79007_HEIGHT 600
#define EK79007_HSYNC 10
#define EK79007_HBP 160
#define EK79007_HFP 160
#define EK79007_VSYNC 1
#define EK79007_VBP 23
#define EK79007_VFP 12
#define EK79007_BPP 24
#define EK79007_DPI_CLOCK_MHZ 48

/* Keep the currently verified diagnostic behavior.  The panel BIST and the
 * DSI host vertical color bars intentionally remain enabled until the bridge
 * and continuous DW-GDMA framebuffer path have separate hardware coverage.
 */

#define EK79007_ENABLE_BIST 1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ek79007_init_cmd_s {
  uint8_t cmd;
  uint8_t data;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ek79007_init_cmd_s g_ek79007_init[] = {
#if EK79007_ENABLE_BIST
  {0xb1, 0x08},
#endif
  {0x80, 0x8b}, {0x81, 0x78}, {0x82, 0x84}, {0x83, 0x88},
  {0x84, 0xa8}, {0x85, 0xe3}, {0x86, 0x88}, {0x3a, 0x77},
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ek79007_backlight(bool on) {
  esp_configgpio(BOARD_LCD_BACKLIGHT, OUTPUT);
  esp_gpiowrite(BOARD_LCD_BACKLIGHT, on);
}

static int ek79007_dcs_write(FAR struct mipi_dsi_device* device, uint8_t cmd,
                             FAR const uint8_t* data, size_t len) {
  ssize_t ret;

  syslog(LOG_INFO, "MIPI: DCS write cmd=%02x len=%u\n", cmd, (unsigned int)len);
  ret = mipi_dsi_dcs_write(device, cmd, data, len);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO, "MIPI: DCS write cmd=%02x result=%ld\n",
         cmd, (long)ret);
  return ret < 0 ? (int)ret : OK;
}

static FAR struct mipi_dsi_device* ek79007_initialize(
  FAR struct mipi_dsi_host* host) {
  FAR struct mipi_dsi_device* device;
  uint8_t id[4] = {0};

  uint8_t pwr = 0;
  unsigned int i;
  uint8_t data;
  int ret;

  if (host == NULL) {
    return NULL;
  }

  ek79007_backlight(false);

  device = mipi_dsi_device_register(host, EK79007_NAME, 0);
  if (device == NULL) {
    syslog(LOG_ERR, "ERROR: EK79007 device registration failed\n");
    return NULL;
  }

  device->lanes = EK79007_LANES;
  device->format = MIPI_DSI_FMT_RGB888;
  device->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
  device->hs_rate = EK79007_LANE_RATE_MBPS * 1000000UL;
  device->lp_rate = 0;

  ret = mipi_dsi_attach(device);
  if (ret < 0) {
    syslog(LOG_ERR, "ERROR: EK79007 attach failed: %d\n", ret);
    return NULL;
  }

  syslog(LOG_INFO, "MIPI: initializing EK79007 panel\n");
  syslog(LOG_INFO, "MIPI: configuring EK79007 reset GPIO%d\n", BOARD_LCD_RST);
  esp_configgpio(BOARD_LCD_RST, OUTPUT);

  /* Keep the lanes in LP11 around GRB.  The delays cover the panel's
   * required power stabilization, reset pulse and >=55 ms post-reset wait.
   */

  esp_gpiowrite(BOARD_LCD_RST, true);
  up_udelay(20000);
  esp_gpiowrite(BOARD_LCD_RST, false);
  up_udelay(30000);
  esp_gpiowrite(BOARD_LCD_RST, true);
  up_udelay(120000);

  ret = ek79007_dcs_write(device, 0x00, NULL, 0);
  if (ret < 0) {
    return NULL;
  }

  up_udelay(2000);
  data = 0x10; /* Select the panel's two-lane mode. */
  ret = ek79007_dcs_write(device, 0xb2, &data, 1);
  if (ret < 0) {
    return NULL;
  }

  for (i = 0; i < sizeof(g_ek79007_init) / sizeof(g_ek79007_init[0]); i++) {
    ret = ek79007_dcs_write(device, g_ek79007_init[i].cmd,
                            &g_ek79007_init[i].data, 1);
    if (ret < 0) {
      return NULL;
    }
  }

#if EK79007_ENABLE_BIST
  syslog(LOG_INFO, "MIPI: EK79007 internal BIST enabled\n");
#endif

  ret = ek79007_dcs_write(device, 0x11, NULL, 0);
  if (ret < 0) {
    return NULL;
  }

  up_udelay(120000);
  ret = ek79007_dcs_write(device, 0x29, NULL, 0);
  if (ret < 0) {
    return NULL;
  }

  up_udelay(20000);

  ret = esp_mipi_dsi_dcs_read(device, 0x04, id, sizeof(id));
  syslog(ret > 0 ? LOG_INFO : LOG_WARNING,
         "MIPI: Display ID read: ret=%d id=%02x %02x %02x %02x\n", ret, id[0],
         id[1], id[2], id[3]);

  ret = esp_mipi_dsi_dcs_read(device, 0x0a, &pwr, 1);
  syslog(ret > 0 ? LOG_INFO : LOG_WARNING,
         "MIPI: Power mode read: ret=%d val=%02x\n", ret, pwr);

  return device;
}

static const struct esp_mipi_dsi_config_s g_ek79007_config = {
  .lanes = EK79007_LANES,
  .lane_rate_mbps = EK79007_LANE_RATE_MBPS,
  .width = EK79007_WIDTH,
  .height = EK79007_HEIGHT,
  .hsync = EK79007_HSYNC,
  .hbp = EK79007_HBP,
  .hfp = EK79007_HFP,
  .vsync = EK79007_VSYNC,
  .vbp = EK79007_VBP,
  .vfp = EK79007_VFP,
  .bpp = EK79007_BPP,
  .format = MIPI_DSI_FMT_RGB888,
  .dpi_clock_mhz = EK79007_DPI_CLOCK_MHZ,
  .use_test_pattern = true,
  .panel_initialize = ek79007_initialize,
  .backlight = ek79007_backlight,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_mipi_dsi_initialize(void) {
  return esp_mipi_dsi_set_config(&g_ek79007_config);
}
