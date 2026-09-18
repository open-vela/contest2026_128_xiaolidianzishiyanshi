/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_mipi_dsi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/video/mipi_display.h>

#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_private/esp_psram_extram.h"
#include "esp_private/periph_ctrl.h"
#include "espressif/esp_irq.h"
#include "hal/axi_icm_ll.h"
#include "hal/cache_ll.h"
#include "hal/clk_gate_ll.h"
#include "hal/config.h"
#include "hal/dw_gdma_hal.h"
#include "hal/dw_gdma_ll.h"
#include "hal/ldo_ll.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_ll.h"
#include "hal/mipi_dsi_phy_ll.h"
#include "soc/mipi_dsi_bridge_reg.h"

#include "esp_mipi_dsi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DSI_BUS 0
#define DSI_POLL_LOOPS 100000
#define DSI_DMA_CHANNEL 0
#define DSI_DMA_ERROR_EVENTS                                                \
  (DW_GDMA_LL_CHANNEL_EVENT_SRC_DEC_ERR |                                   \
   DW_GDMA_LL_CHANNEL_EVENT_DST_DEC_ERR |                                   \
   DW_GDMA_LL_CHANNEL_EVENT_SRC_SLV_ERR |                                   \
   DW_GDMA_LL_CHANNEL_EVENT_DST_SLV_ERR |                                   \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_DEC_ERR |                                \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_DEC_ERR |                                \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_SLV_ERR |                                \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_SLV_ERR |                                \
   DW_GDMA_LL_CHANNEL_EVENT_SHADOWREG_OR_LLI_INVALID_ERR)
#define DSI_DMA_EVENTS                                                      \
  (DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE | DSI_DMA_ERROR_EVENTS)

#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300
#define DSI_PHY_REF_HZ 40000000
#define DSI_PHY_PLLREF_CLK_SRC MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT
#else
#define DSI_PHY_REF_HZ 20000000
#define DSI_PHY_PLLREF_CLK_SRC MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_dsi_s {
  struct mipi_dsi_host host;
  struct mipi_dsi_device* panel;
  mipi_dsi_hal_context_t hal;
  dw_gdma_hal_context_t dma;
  mutex_t lock;
  int dma_cpuint;
  volatile uint32_t dma_error;
  volatile uint8_t current_fb;
  volatile uint8_t pending_fb;
  struct fb_area_s last_update;
  bool last_update_valid;
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_dsi_dma_initialize(struct esp_dsi_s* priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_dsi_s g_dsi = {
  .lock = NXMUTEX_INITIALIZER,
  .dma_cpuint = -1,
};

static dw_gdma_link_list_item_t g_dsi_dma_lli;

static FAR uint8_t* g_framebuffer;
static FAR const struct esp_mipi_dsi_config_s* g_config;

static struct fb_videoinfo_s g_vinfo = {
  .nplanes = 1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_dsi_getvideoinfo(struct fb_vtable_s* vtable,
                                struct fb_videoinfo_s* vinfo) {
  (void)vtable;

  if (vinfo == NULL) {
    return -EINVAL;
  }

  memcpy(vinfo, &g_vinfo, sizeof(*vinfo));
  return OK;
}

static int esp_dsi_getplaneinfo(struct fb_vtable_s* vtable, int planeno,
                                struct fb_planeinfo_s* pinfo) {
  size_t fb_size;
  size_t stride;

  (void)vtable;

  if ((planeno != 0 && planeno != 1) || pinfo == NULL) {
    return -EINVAL;
  }

  stride = (size_t)g_config->width * g_config->bpp / 8;
  fb_size = stride * g_config->height;
  pinfo->fbmem = g_framebuffer;
  pinfo->fblen = fb_size * 2;
  pinfo->stride = stride;
  pinfo->display = planeno;
  pinfo->bpp = g_config->bpp;
  pinfo->xres_virtual = g_config->width;
  pinfo->yres_virtual = g_config->height * 2;
  pinfo->xoffset = 0;
  pinfo->yoffset = g_dsi.current_fb * g_config->height;
  return OK;
}

static int esp_dsi_pandisplay(struct fb_vtable_s* vtable,
                              struct fb_planeinfo_s* pinfo) {
  uint32_t fb;

  (void)vtable;

  if (pinfo == NULL || pinfo->xoffset != 0 ||
      (pinfo->yoffset != 0 && pinfo->yoffset != g_config->height)) {
    return -EINVAL;
  }

  fb = pinfo->yoffset / g_config->height;

  /* Commit the completed LVGL draw buffer at the next frame boundary.  The
   * DMA completion ISR owns the actual descriptor switch, so the buffer
   * currently being scanned is never changed halfway through a frame.
   */

  g_dsi.pending_fb = fb;
  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int esp_dsi_cache_sync_area(uint8_t fb,
                                   const struct fb_area_s* area) {
  uintptr_t start;
  size_t bytes_per_pixel = g_config->bpp / 8;
  size_t stride = (size_t)g_config->width * bytes_per_pixel;
  size_t fb_size = stride * g_config->height;
  size_t row_len = (size_t)area->w * bytes_per_pixel;
  uint32_t row;

  start = (uintptr_t)g_framebuffer + (size_t)fb * fb_size +
          (size_t)area->y * stride + (size_t)area->x * bytes_per_pixel;

  for (row = 0; row < area->h; row++) {
    if (esp_cache_msync((void*)(start + row * stride), row_len,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
      return -EIO;
    }
  }

  return OK;
}

static int esp_dsi_updatearea(struct fb_vtable_s* vtable,
                              const struct fb_area_s* area) {
  struct fb_area_s update;
  uint32_t fb;

  (void)vtable;

  if (area == NULL || area->w == 0 || area->h == 0 ||
      area->x >= g_config->width || area->y >= g_config->height * 2) {
    return -EINVAL;
  }

  fb = area->y / g_config->height;
  update.x = area->x;
  update.y = area->y % g_config->height;
  update.w = area->x + area->w > g_config->width ?
               g_config->width - area->x : area->w;
  update.h = update.y + area->h > g_config->height ?
               g_config->height - update.y : area->h;

  /* LVGL's double-buffered DIRECT mode first copies the previous frame's
   * dirty areas into the new off-screen buffer, then renders this frame's
   * invalid areas.  FBIO_UPDATE reports only the latter.  Write back both
   * rectangles or the automatic buffer synchronization remains cache-only
   * and the display DMA sees stale PSRAM pixels from the older frame.
   */

  if (g_dsi.last_update_valid &&
      esp_dsi_cache_sync_area(fb, &g_dsi.last_update) < 0) {
    return -EIO;
  }

  if (esp_dsi_cache_sync_area(fb, &update) < 0) {
    return -EIO;
  }

  g_dsi.last_update = update;
  g_dsi.last_update_valid = true;

  if (g_dsi.dma_error != 0) {
    syslog(LOG_ERR, "ERROR: MIPI DW-GDMA stopped (status=%08lx)\n",
           (unsigned long)g_dsi.dma_error);
    return -EIO;
  }

  return OK;
}
#endif

static struct fb_vtable_s g_fbops = {
  .getvideoinfo = esp_dsi_getvideoinfo,
  .getplaneinfo = esp_dsi_getplaneinfo,
  .pandisplay = esp_dsi_pandisplay,
#ifdef CONFIG_FB_UPDATE
  .updatearea = esp_dsi_updatearea,
#endif
};

static int esp_dsi_wait_cmd_space(struct esp_dsi_s* priv) {
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++) {
    if (!mipi_dsi_host_ll_gen_is_cmd_fifo_full(priv->hal.host)) {
      return OK;
    }
  }

  return -ETIMEDOUT;
}

static int esp_dsi_wait_payload_space(struct esp_dsi_s* priv) {
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++) {
    if (!mipi_dsi_host_ll_gen_is_write_fifo_full(priv->hal.host)) {
      return OK;
    }
  }

  return -ETIMEDOUT;
}

static int esp_dsi_wait_tx_done(struct esp_dsi_s* priv) {
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++) {
    if (mipi_dsi_host_ll_gen_is_cmd_fifo_empty(priv->hal.host) &&
        mipi_dsi_host_ll_gen_is_write_fifo_empty(priv->hal.host)) {
      return OK;
    }
  }

  syslog(LOG_ERR,
         "ERROR: MIPI command transmit timeout (status=%08lx int=%08lx)\n",
         (unsigned long)priv->hal.host->cmd_pkt_status.val,
         (unsigned long)priv->hal.host->int_st0.val);
  return -ETIMEDOUT;
}

static bool esp_dsi_is_read_packet(uint8_t type) {
  switch (type) {
    case MIPI_DSI_GENERIC_READ_0_PARAM:
    case MIPI_DSI_GENERIC_READ_1_PARAM:
    case MIPI_DSI_GENERIC_READ_2_PARAM:
    case MIPI_DSI_DCS_READ_0_PARAM:
      return true;

    default:
      return false;
  }
}

static ssize_t esp_dsi_transfer(struct mipi_dsi_host* host,
                                const struct mipi_dsi_msg* msg) {
  struct esp_dsi_s* priv = (struct esp_dsi_s*)host;
  const uint8_t* buf;
  uint32_t word;
  size_t left;
  uint16_t header = 0;
  int ret;

  if (host == NULL || msg == NULL || msg->channel > 3 ||
      msg->tx_len > UINT16_MAX || (msg->tx_len != 0 && msg->tx_buf == NULL)) {
    return -EINVAL;
  }

  buf = msg->tx_buf;

  /* Decide transfer direction from the packet data type.  Some NuttX DSI
   * write helpers leave rx_len/rx_buf uninitialized, so consulting rx_len
   * here can randomly reject a valid write with -ENOTSUP.
   */

  if (esp_dsi_is_read_packet(msg->type)) {
    return -ENOTSUP;
  }

  syslog(LOG_INFO, "MIPI: transfer locking host\n");
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0) {
    return ret;
  }

  syslog(LOG_INFO, "MIPI: transfer host locked\n");

  if (msg->tx_len > 2) {
    left = msg->tx_len;
    while (left != 0) {
      size_t ncopy = left > sizeof(word) ? sizeof(word) : left;
      word = 0;
      memcpy(&word, buf, ncopy);
      ret = esp_dsi_wait_payload_space(priv);
      if (ret < 0) {
        goto out;
      }

      mipi_dsi_host_ll_gen_write_payload_fifo(priv->hal.host, word);
      buf += ncopy;
      left -= ncopy;
    }

    header = msg->tx_len;
  } else if (msg->tx_len != 0) {
    header = buf[0];
    if (msg->tx_len == 2) {
      header |= (uint16_t)buf[1] << 8;
    }
  }

  ret = esp_dsi_wait_cmd_space(priv);
  if (ret >= 0) {
    syslog(LOG_INFO, "MIPI: writing packet header type=%02x hdr=%04x\n",
           msg->type, header);
    mipi_dsi_host_ll_gen_set_packet_header(
      priv->hal.host, msg->channel, msg->type, header >> 8, header & 0xff);
    ret = esp_dsi_wait_tx_done(priv);
    if (ret >= 0) {
      /* Keep vendor writes serialized beyond the FIFO boundary. */

      up_udelay(1000);
      ret = msg->tx_len;
    }
  }

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_dsi_attach(struct mipi_dsi_host* host,
                          struct mipi_dsi_device* device) {
  (void)host;

  return g_config != NULL && device->lanes == g_config->lanes ? OK : -EINVAL;
}

static int esp_dsi_detach(struct mipi_dsi_host* host,
                          struct mipi_dsi_device* device) {
  (void)host;
  (void)device;

  return OK;
}

static const struct mipi_dsi_host_ops g_host_ops = {
  .attach = esp_dsi_attach,
  .detach = esp_dsi_detach,
  .transfer = esp_dsi_transfer,
};

/* Single-shot DCS read with BTA and full RX cleanup.
 *
 * This reads one DCS response from the panel.  BTA is enabled only for the
 * duration of this call and disabled again on exit, so it does not interfere
 * with the normal LPDT command path used for panel initialization.
 *
 * Returns the number of bytes read (1-4) on success, or a negative errno.
 */

int esp_mipi_dsi_dcs_read(struct mipi_dsi_device* device, uint8_t cmd,
                          uint8_t* buf, size_t max_len) {
  struct esp_dsi_s* priv = &g_dsi;
  unsigned int n;
  uint32_t val;
  int ret;

  if (device == NULL || device->host != &priv->host || buf == NULL ||
      max_len == 0) {
    return -EINVAL;
  }

  /* Drain any stale data in the RX FIFO before starting a new read. */

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host)) {
    (void)mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
  }

  /* Enable BTA for this read only. */

  mipi_dsi_host_ll_enable_bta(priv->hal.host, true);

  /* Send DCS read request: DCS_READ_0 (0x06) with the command byte. */

  mipi_dsi_host_ll_gen_set_packet_header(priv->hal.host, 0,
                                         MIPI_DSI_DT_DCS_READ_0, 0, cmd);

  /* Wait for the read response to arrive.  The host sets the read-command-
   * busy flag while waiting for the panel response, and data appears in the
   * RX FIFO once the BTA turnaround completes.
   */

  ret = -ETIMEDOUT;
  for (n = 0; n < DSI_POLL_LOOPS; n++) {
    if (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host)) {
      val = mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
      ret = 1;
      break;
    }

    if (!mipi_dsi_host_ll_gen_is_read_cmd_busy(priv->hal.host) &&
        mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host)) {
      /* Command completed but no data — panel did not respond. */

      ret = -ENODATA;
      break;
    }
  }

  /* Disable BTA immediately after the read. */

  mipi_dsi_host_ll_enable_bta(priv->hal.host, false);

  if (ret > 0) {
    /* The RX FIFO word contains up to 4 bytes in little-endian order. */

    size_t ncopy = max_len < 4 ? max_len : 4;
    memcpy(buf, &val, ncopy);
    syslog(LOG_INFO, "MIPI: DCS read cmd=%02x data=%02x %02x %02x %02x\n", cmd,
           buf[0], ncopy > 1 ? buf[1] : 0, ncopy > 2 ? buf[2] : 0,
           ncopy > 3 ? buf[3] : 0);
  } else {
    syslog(LOG_ERR,
           "MIPI: DCS read cmd=%02x failed ret=%d "
           "int_st0=%08lx int_st1=%08lx phy=%08lx\n",
           cmd, ret, (unsigned long)priv->hal.host->int_st0.val,
           (unsigned long)priv->hal.host->int_st1.val,
           (unsigned long)priv->hal.host->phy_status.val);
  }

  /* Drain the RX FIFO and clear any residual interrupt flags so they do not
   * affect the subsequent video mode operation.
   */

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host)) {
    (void)mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
  }

  /* Write-1-to-clear on interrupt status registers. */

  priv->hal.host->int_st0.val = priv->hal.host->int_st0.val;
  priv->hal.host->int_st1.val = priv->hal.host->int_st1.val;

  return ret;
}

static inline dw_gdma_link_list_item_t* esp_dsi_dma_lli_noncache(void) {
  return (dw_gdma_link_list_item_t*)CACHE_LL_L2MEM_NON_CACHE_ADDR(
    &g_dsi_dma_lli);
}

static void esp_dsi_dma_reload(struct esp_dsi_s* priv) {
  dw_gdma_dev_t* dma = priv->dma.dev;
  dw_gdma_link_list_item_t* lli = esp_dsi_dma_lli_noncache();
  size_t fb_size = (size_t)g_config->width * g_config->height *
                   g_config->bpp / 8;
  uint8_t fb = priv->pending_fb;

  /* DW-GDMA clears the valid marker after consuming the descriptor.  Restore
   * it, reload the single-item list, and restart immediately so the DPI bridge
   * receives a continuous video stream without a worker thread or framebuffer
   * copies.  This is the same restart sequence used by ESP-IDF's DPI panel.
   */

  dw_gdma_ll_lli_set_src_addr(
    lli, (uint32_t)(g_framebuffer + (size_t)fb * fb_size));
  dw_gdma_ll_lli_set_block_markers(lli, false, true, true);
  dw_gdma_ll_channel_set_link_list_master_port(
    dma, DSI_DMA_CHANNEL, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_channel_set_link_list_head_addr(
    dma, DSI_DMA_CHANNEL, (uint32_t)&g_dsi_dma_lli);
  dw_gdma_ll_channel_enable(dma, DSI_DMA_CHANNEL, true);
  priv->current_fb = fb;
}

static int esp_dsi_dma_interrupt(int irq, void* context, void* arg) {
  struct esp_dsi_s* priv = arg;
  dw_gdma_dev_t* dma = priv->dma.dev;
  uint32_t status;

  (void)irq;
  (void)context;

  status = dw_gdma_ll_channel_get_intr_status(dma, DSI_DMA_CHANNEL);
  dw_gdma_ll_channel_clear_intr(dma, DSI_DMA_CHANNEL, status);

  if ((status & DSI_DMA_ERROR_EVENTS) != 0) {
    priv->dma_error |= status & DSI_DMA_ERROR_EVENTS;
  } else if ((status & DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE) != 0) {
    esp_dsi_dma_reload(priv);
    fb_notify_vsync(&g_fbops);
    (void)fb_remove_paninfo(&g_fbops, FB_NO_OVERLAY);
  }

  return OK;
}

static int esp_dsi_dma_initialize(struct esp_dsi_s* priv) {
  dw_gdma_link_list_item_t* lli;
  dw_gdma_dev_t* dma;
  size_t fb_size;
  int cpuint;
  int ret;

  /* The ESP-IDF upper DW-GDMA driver depends on FreeRTOS.  Reproduce its
   * channel and one-item linked-list setup here with the NuttX interrupt
   * router, while retaining the descriptor layout and programming sequence.
   */

  memset(&g_dsi_dma_lli, 0, sizeof(g_dsi_dma_lli));
  ret = esp_cache_msync(&g_dsi_dma_lli, sizeof(g_dsi_dma_lli),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret != ESP_OK) {
    return -EIO;
  }

  lli = esp_dsi_dma_lli_noncache();
  fb_size = (size_t)g_config->width * g_config->height * g_config->bpp / 8;
  dw_gdma_ll_lli_set_next_item_addr(lli, 0);
  dw_gdma_ll_lli_set_link_list_master_port(
    lli, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_lli_set_src_addr(lli, (uint32_t)g_framebuffer);
  dw_gdma_ll_lli_set_dst_addr(lli, MIPI_DSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_trans_block_size(lli, fb_size / 8);
  dw_gdma_ll_lli_set_src_master_port(lli, (intptr_t)g_framebuffer);
  dw_gdma_ll_lli_set_dst_master_port(lli, MIPI_DSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_src_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_dst_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_src_burst_items(lli, DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_lli_set_dst_burst_items(lli, DW_GDMA_BURST_ITEMS_256);
  dw_gdma_ll_lli_set_src_burst_mode(lli, DW_GDMA_BURST_MODE_INCREMENT);
  dw_gdma_ll_lli_set_dst_burst_mode(lli, DW_GDMA_BURST_MODE_FIXED);
  dw_gdma_ll_lli_set_src_burst_len(lli, 16);
  dw_gdma_ll_lli_set_dst_burst_len(lli, 16);
  dw_gdma_ll_lli_enable_src_periph_status_write_back(lli, false);
  dw_gdma_ll_lli_enable_dst_periph_status_write_back(lli, false);
  dw_gdma_ll_lli_set_block_markers(lli, false, true, true);

  PERIPH_RCC_ATOMIC() {
    dw_gdma_ll_enable_bus_clock(0, true);
    dw_gdma_ll_reset_register(0);
  }

  dw_gdma_hal_init(&priv->dma, NULL);
  dma = priv->dma.dev;

  /* Display fetches are latency-sensitive.  Give the DW-GDMA memory master
   * maximum AXI read priority so CPU cache writeback cannot starve the DPI
   * FIFO while LVGL publishes a frame from PSRAM.
   */

  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, 15);
  dw_gdma_ll_channel_set_trans_flow(dma, DSI_DMA_CHANNEL, DW_GDMA_ROLE_MEM,
                                    DW_GDMA_ROLE_PERIPH_DSI,
                                    DW_GDMA_FLOW_CTRL_SELF);
  dw_gdma_ll_channel_set_src_multi_block_type(
    dma, DSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_dst_multi_block_type(
    dma, DSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_src_handshake_interface(
    dma, DSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_interface(
    dma, DSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_periph(
    dma, DSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_DSI);
  dw_gdma_ll_channel_set_priority(dma, DSI_DMA_CHANNEL, 1);
  dw_gdma_ll_channel_set_src_outstanding_limit(dma, DSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_dst_outstanding_limit(dma, DSI_DMA_CHANNEL, 2);
  dw_gdma_ll_channel_set_src_periph_status_addr(dma, DSI_DMA_CHANNEL, 0);
  dw_gdma_ll_channel_set_dst_periph_status_addr(dma, DSI_DMA_CHANNEL, 0);
  dw_gdma_ll_channel_enable_intr_propagation(dma, DSI_DMA_CHANNEL, UINT32_MAX,
                                             false);
  dw_gdma_ll_channel_clear_intr(dma, DSI_DMA_CHANNEL, UINT32_MAX);
  dw_gdma_ll_channel_enable_intr_generation(dma, DSI_DMA_CHANNEL, UINT32_MAX,
                                            true);

  /* Interrupt routing is CPU-local, so prevent migration/preemption between
   * allocating the CPU interrupt and enabling its peripheral IRQ.
   */

  sched_lock();
  cpuint = esp_setup_irq(DW_GDMA_INTR_SOURCE, ESP_IRQ_PRIORITY_DEFAULT,
                         ESP_IRQ_TRIGGER_LEVEL, esp_dsi_dma_interrupt, priv);
  if (cpuint >= 0) {
    priv->dma_cpuint = cpuint;
    priv->dma_error = 0;
    dw_gdma_ll_channel_enable_intr_propagation(
      dma, DSI_DMA_CHANNEL, DSI_DMA_EVENTS, true);
    up_enable_irq(ESP_IRQ_DW_GDMA);
  }

  sched_unlock();
  if (cpuint < 0) {
    dw_gdma_hal_deinit(&priv->dma);
    return cpuint;
  }

  priv->current_fb = 0;
  priv->pending_fb = 0;
  priv->last_update_valid = false;
  esp_dsi_dma_reload(priv);
  return OK;
}

static int esp_dsi_hardware_initialize(struct esp_dsi_s* priv) {
  mipi_dsi_hal_config_t hal_cfg = {
    .bus_id = DSI_BUS,
    .lane_bit_rate_mbps = g_config->lane_rate_mbps,
    .num_data_lanes = g_config->lanes};

  irqstate_t flags;
  uint8_t dref;
  uint8_t mul;
  bool use_rail;
  uint32_t div;
  unsigned int n;
  int ret;

  syslog(LOG_INFO, "MIPI: enabling 2.5V PHY LDO\n");
  ldo_ll_voltage_to_dref_mul(LDO_ID2UNIT(3), 2500, &dref, &mul, &use_rail);
  flags = enter_critical_section();
  ldo_ll_adjust_voltage(LDO_ID2UNIT(3), dref, mul, use_rail);
  ldo_ll_set_owner(LDO_ID2UNIT(3), LDO_LL_UNIT_OWNER_SW);
  ldo_ll_enable_ripple_suppression(LDO_ID2UNIT(3), true);
  ldo_ll_enable(LDO_ID2UNIT(3), true);
  leave_critical_section(flags);
  up_udelay(10000);
  syslog(LOG_INFO, "MIPI: 2.5V PHY LDO enabled\n");

  syslog(LOG_INFO, "MIPI: enabling DSI peripheral clocks\n");
  PERIPH_RCC_ATOMIC() {
    clk_gate_ll_ref_20m_clk_en(true);
    mipi_dsi_ll_enable_bus_clock(DSI_BUS, true);
    mipi_dsi_ll_reset_register(DSI_BUS);
    mipi_dsi_ll_set_phy_config_clock_source(DSI_BUS,
                                            MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT);
    mipi_dsi_ll_enable_phy_config_clock(DSI_BUS, true);
    mipi_dsi_ll_set_phy_pllref_clock_source(DSI_BUS, DSI_PHY_PLLREF_CLK_SRC);
    mipi_dsi_ll_set_phy_pll_ref_clock_div(DSI_BUS, 1);
    mipi_dsi_ll_enable_phy_pllref_clock(DSI_BUS, true);
  }

  syslog(LOG_INFO, "MIPI: DSI and REF_20M clocks enabled\n");

  syslog(LOG_INFO, "MIPI: configuring DSI PHY and PLL\n");
  priv->hal.host = MIPI_DSI_LL_GET_HOST(hal_cfg.bus_id);
  priv->hal.bridge = MIPI_DSI_LL_GET_BRG(hal_cfg.bus_id);
  mipi_dsi_phy_ll_set_data_lane_number(priv->hal.host, hal_cfg.num_data_lanes);
  syslog(LOG_INFO, "MIPI: PHY lane count configured\n");
  mipi_dsi_host_ll_power_on_off(priv->hal.host, true);
  syslog(LOG_INFO, "MIPI: DSI host powered on\n");
  mipi_dsi_phy_ll_power_on_off(priv->hal.host, true);
  syslog(LOG_INFO, "MIPI: DSI PHY powered on\n");
  mipi_dsi_phy_ll_reset(priv->hal.host);
  syslog(LOG_INFO, "MIPI: DSI PHY reset\n");
  mipi_dsi_phy_ll_enable_clock_lane(priv->hal.host, true);
  syslog(LOG_INFO, "MIPI: DSI clock lane enabled\n");
  mipi_dsi_phy_ll_force_pll(priv->hal.host, true);
  syslog(LOG_INFO, "MIPI: DSI PHY PLL forced on\n");
  mipi_dsi_brg_ll_reset(priv->hal.bridge);
  syslog(LOG_INFO, "MIPI: DSI bridge reset\n");
  mipi_dsi_hal_configure_phy_pll(&priv->hal, DSI_PHY_REF_HZ,
                                 g_config->lane_rate_mbps);
  syslog(LOG_INFO, "MIPI: DSI PHY PLL configured\n");

  syslog(LOG_INFO, "MIPI: waiting for PHY PLL lock (status=%08lx)\n",
         (unsigned long)priv->hal.host->phy_status.val);
  for (n = 0; n < 500; n++) {
    if (mipi_dsi_phy_ll_is_pll_locked(priv->hal.host)) {
      break;
    }

    up_udelay(1000);
  }

  if (n == 500) {
    syslog(LOG_ERR, "ERROR: MIPI PHY PLL lock timeout (status=%08lx)\n",
           (unsigned long)priv->hal.host->phy_status.val);
    return -ETIMEDOUT;
  }

  syslog(LOG_INFO, "MIPI: PHY PLL locked\n");

  for (n = 0; n < 500; n++) {
    if (mipi_dsi_phy_ll_are_lanes_stopped(priv->hal.host, g_config->lanes)) {
      break;
    }

    up_udelay(1000);
  }

  if (n == 500) {
    syslog(LOG_ERR,
           "ERROR: MIPI PHY lanes did not enter stop state "
           "(status=%08lx)\n",
           (unsigned long)priv->hal.host->phy_status.val);
    return -ETIMEDOUT;
  }

  syslog(LOG_INFO, "MIPI: PHY lanes stopped\n");

  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
  mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host,
                                        MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
  mipi_dsi_phy_ll_set_switch_time(priv->hal.host, 50, 104, 46, 128);
  mipi_dsi_host_ll_enable_rx_crc(priv->hal.host, true);
  mipi_dsi_host_ll_enable_rx_ecc(priv->hal.host, true);
  mipi_dsi_host_ll_enable_tx_eotp(priv->hal.host, true, false);
  mipi_dsi_host_ll_set_timeout_clock_division(priv->hal.host, 13);
  mipi_dsi_host_ll_set_escape_clock_division(priv->hal.host, 7);
  mipi_dsi_host_ll_set_timeout_count(priv->hal.host, 0, 0, 0, 0, 0, 0, 0);
  mipi_dsi_phy_ll_set_max_read_time(priv->hal.host, 6000);
  mipi_dsi_phy_ll_set_stop_wait_time(priv->hal.host, 0x3f);

  /* Match ESP-IDF's DBI panel IO setup.  Panel initialization commands are
   * LPDT transactions; leaving cmd_mode_cfg at reset defaults sends them as
   * HS packets, which can empty the host FIFO without reaching the panel.
   */

  mipi_dsi_host_ll_enable_te_ack(priv->hal.host, false);
  mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
  mipi_dsi_host_ll_enable_bta(priv->hal.host, false);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(priv->hal.host, 0,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(priv->hal.host, 1,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(priv->hal.host, 2,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_long_wr_speed_mode(priv->hal.host,
                                              MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(priv->hal.host, 0,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(priv->hal.host, 1,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(priv->hal.host, 2,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(priv->hal.host, 0,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(priv->hal.host, 1,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(priv->hal.host,
                                              MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(priv->hal.host, 0,
                                               MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_mrps_speed_mode(priv->hal.host,
                                       MIPI_DSI_LL_TRANS_SPEED_LP);
  syslog(LOG_INFO, "MIPI: DBI commands configured for LP mode\n");

  priv->host.bus = DSI_BUS;
  priv->host.ops = &g_host_ops;
  ret = mipi_dsi_host_register(&priv->host);
  if (ret < 0) {
    return ret;
  }

  priv->panel = g_config->panel_initialize(&priv->host);
  if (priv->panel == NULL) {
    return -ENODEV;
  }

  priv->hal.expect_dpi_clock_freq_mhz = (float)g_config->dpi_clock_mhz;
  div = mipi_dsi_hal_host_dpi_calculate_divider(&priv->hal, 240.0f,
                                                (float)g_config->dpi_clock_mhz);
  PERIPH_RCC_ATOMIC() {
    clk_gate_ll_ref_240m_clk_en(true);
    mipi_dsi_ll_set_dpi_clock_source(DSI_BUS, MIPI_DSI_DPI_CLK_SRC_DEFAULT);
    mipi_dsi_ll_set_dpi_clock_div(DSI_BUS, div);
    mipi_dsi_ll_enable_dpi_clock(DSI_BUS, true);
  }

  syslog(LOG_INFO, "MIPI: REF_240M and DPI clocks enabled (div=%lu)\n",
         (unsigned long)div);

  mipi_dsi_host_ll_dpi_set_vcid(priv->hal.host, 0);

  mipi_dsi_host_ll_dpi_set_color_coding(priv->hal.host, LCD_COLOR_FMT_RGB888,
                                        0);
  mipi_dsi_host_ll_dpi_set_timing_polarity(priv->hal.host, false, false, false,
                                           false, false);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(priv->hal.host, true, true);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(priv->hal.host, true, true,
                                                 true, true);
  mipi_dsi_host_ll_dpi_enable_lp_command(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_set_video_burst_type(
    priv->hal.host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);
  mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(priv->hal.host,
                                                  g_config->width);
  mipi_dsi_host_ll_dpi_set_trunks_num(priv->hal.host, 0);
  mipi_dsi_host_ll_dpi_set_null_packet_size(priv->hal.host, 0);
  mipi_dsi_hal_host_dpi_set_horizontal_timing(
    &priv->hal, g_config->hsync, g_config->hbp, g_config->width, g_config->hfp);
  mipi_dsi_hal_host_dpi_set_vertical_timing(&priv->hal, g_config->vsync,
                                            g_config->vbp, g_config->height,
                                            g_config->vfp);
  mipi_dsi_brg_ll_set_num_pixel_bits(
    priv->hal.bridge, g_config->width * g_config->height * g_config->bpp);
  mipi_dsi_brg_ll_set_underrun_discard_count(priv->hal.bridge, g_config->width);
  mipi_dsi_brg_ll_set_input_color_format(priv->hal.bridge,
                                         LCD_COLOR_FMT_RGB888);
  mipi_dsi_brg_ll_set_output_color_format(priv->hal.bridge,
                                          LCD_COLOR_FMT_RGB888, 0);
  mipi_dsi_brg_ll_set_flow_controller(priv->hal.bridge,
                                      MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
  mipi_dsi_brg_ll_set_multi_block_number(priv->hal.bridge, 1);
  mipi_dsi_brg_ll_set_burst_len(priv->hal.bridge, 256);
  mipi_dsi_brg_ll_set_empty_threshold(priv->hal.bridge, 768);
  mipi_dsi_brg_ll_enable(priv->hal.bridge, true);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);

  /* This panel is operated entirely in HS video mode.  Its command path does
   * not provide a reliable BTA response, so video must not wait for a frame
   * acknowledgement before starting the next frame.
   */

  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(priv->hal.host, false,
                                                   false);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(priv->hal.host, false, false,
                                                 false, false);
  mipi_dsi_host_ll_dpi_enable_lp_command(priv->hal.host, false);
  mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host,
                                        MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
  syslog(LOG_INFO, "MIPI: forced HS video mode (no LP blanking)\n");
  if (g_config->use_test_pattern) {
    mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
    mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
    mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                          MIPI_DSI_PATTERN_BAR_VERTICAL);
  } else {
    syslog(LOG_INFO, "MIPI: configuring framebuffer DW-GDMA\n");
    ret = esp_dsi_dma_initialize(priv);
    if (ret < 0) {
      return ret;
    }
  }

  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
  if (!g_config->use_test_pattern) {
    mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
    mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
  }

  g_config->backlight(true);
  syslog(LOG_INFO, "MIPI: %s enabled\n",
         g_config->use_test_pattern ? "vertical color-bar test pattern"
                                    : "video output");
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_mipi_dsi_set_config(const struct esp_mipi_dsi_config_s* config) {
  if (config == NULL || config->lanes == 0 || config->lanes > 2 ||
      config->lane_rate_mbps == 0 || config->width == 0 ||
      config->height == 0 || config->dpi_clock_mhz == 0 || config->bpp != 24 ||
      ((size_t)config->width * config->height * config->bpp / 8) % 8 != 0 ||
      config->format != MIPI_DSI_FMT_RGB888 ||
      config->panel_initialize == NULL || config->backlight == NULL) {
    return -EINVAL;
  }

  if (g_config == config) {
    return OK;
  }

  if (g_dsi.initialized) {
    return -EBUSY;
  }

  g_config = config;
  g_vinfo.fmt = FB_FMT_RGB24;
  g_vinfo.xres = config->width;
  g_vinfo.yres = config->height;
  return OK;
}

int up_fbinitialize(int display) {
  size_t fb_size;
  int ret;

  if (display != 0 || g_config == NULL) {
    return -ENODEV;
  }

  fb_size = (size_t)g_config->width * g_config->height * g_config->bpp / 8;
  if (!g_dsi.initialized) {
    if (g_framebuffer == NULL) {
      g_framebuffer = kmm_memalign(64, fb_size * 2);
      if (g_framebuffer == NULL) {
        syslog(LOG_ERR,
               "ERROR: MIPI framebuffer allocation failed "
               "(%u bytes)\n",
               (unsigned int)(fb_size * 2));
        return -ENOMEM;
      }

      if (!esp_psram_check_ptr_addr(g_framebuffer) ||
          !esp_psram_check_ptr_addr(g_framebuffer + fb_size * 2 - 1)) {
        syslog(LOG_ERR, "ERROR: MIPI framebuffer was not allocated in PSRAM\n");
        kmm_free(g_framebuffer);
        g_framebuffer = NULL;
        return -ENOMEM;
      }
    }

    memset(g_framebuffer, 0, fb_size * 2);
    if (esp_cache_msync(g_framebuffer, fb_size * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK) {
      return -EIO;
    }

    ret = esp_dsi_hardware_initialize(&g_dsi);
    if (ret >= 0) {
      g_dsi.initialized = true;
    }
  } else {
    ret = OK;
  }

  return ret;
}

struct fb_vtable_s* up_fbgetvplane(int display, int vplane) {
  return display == 0 && vplane == 0 ? &g_fbops : NULL;
}

void up_fbuninitialize(int display) {
  if (display == 0 && g_config != NULL) {
    g_config->backlight(false);
  }
}
