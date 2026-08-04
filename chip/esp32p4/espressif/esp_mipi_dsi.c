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
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_private/dw_gdma.h"
#include "hal/clk_gate_ll.h"
#include "hal/ldo_ll.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_ll.h"
#include "hal/mipi_dsi_phy_ll.h"
#include "soc/mipi_dsi_bridge_reg.h"

#include "espressif/esp_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DSI_BUS                 0
#define DSI_LANES               2
#define DSI_LANE_RATE_MBPS      650
#define DSI_PHY_REF_HZ          40000000
#define DSI_POLL_LOOPS          100000

#define LCD_WIDTH               1024
#define LCD_HEIGHT              600
#define LCD_BPP                 24
#define LCD_STRIDE              (LCD_WIDTH * LCD_BPP / 8)
#define LCD_FB_SIZE             (LCD_STRIDE * LCD_HEIGHT)

#define LCD_HSYNC               10
#define LCD_HBP                 160
#define LCD_HFP                 160
#define LCD_VSYNC               1
#define LCD_VBP                 23
#define LCD_VFP                 12

/* Enable EK79007 internal BIST for diagnostic mode.  BIST does not require
 * DCLK and can verify whether the panel is actually receiving commands.
 * The DSI host VPG is also enabled; if BIST shows full-screen patterns,
 * the panel is receiving commands correctly.
 */

#define LCD_EK79007_BIST        1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_dsi_s
{
  struct mipi_dsi_host host;
  struct mipi_dsi_device *panel;
  mipi_dsi_hal_context_t hal;
  mutex_t lock;
  dw_gdma_channel_handle_t dma;
  dw_gdma_link_list_handle_t link;
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_dsi_dma_submit(struct esp_dsi_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_dsi_s g_dsi =
{
  .lock = NXMUTEX_INITIALIZER,
};

static uint8_t g_framebuffer[LCD_FB_SIZE]
  EXT_RAM_BSS_ATTR __attribute__((aligned(64)));

static const struct fb_videoinfo_s g_vinfo =
{
  .fmt = FB_FMT_RGB24,
  .xres = LCD_WIDTH,
  .yres = LCD_HEIGHT,
  .nplanes = 1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_dsi_getvideoinfo(struct fb_vtable_s *vtable,
                                struct fb_videoinfo_s *vinfo)
{
  (void)vtable;

  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  memcpy(vinfo, &g_vinfo, sizeof(*vinfo));
  return OK;
}

static int esp_dsi_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                                struct fb_planeinfo_s *pinfo)
{
  (void)vtable;

  if (planeno != 0 || pinfo == NULL)
    {
      return -EINVAL;
    }

  pinfo->fbmem = g_framebuffer;
  pinfo->fblen = LCD_FB_SIZE;
  pinfo->stride = LCD_STRIDE;
  pinfo->display = 0;
  pinfo->bpp = LCD_BPP;
  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int esp_dsi_updatearea(struct fb_vtable_s *vtable,
                              const struct fb_area_s *area)
{
  uintptr_t start;
  size_t len;

  (void)vtable;

  if (area == NULL || area->x >= LCD_WIDTH || area->y >= LCD_HEIGHT)
    {
      return -EINVAL;
    }

  start = (uintptr_t)g_framebuffer + area->y * LCD_STRIDE;
  len = ((area->y + area->h > LCD_HEIGHT) ? LCD_HEIGHT - area->y :
         area->h) * LCD_STRIDE;
  if (esp_cache_msync((void *)start, len,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                      ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK)
    {
      return -EIO;
    }

  return g_dsi.initialized ? esp_dsi_dma_submit(&g_dsi) : OK;
}
#endif

static struct fb_vtable_s g_fbops =
{
  .getvideoinfo = esp_dsi_getvideoinfo,
  .getplaneinfo = esp_dsi_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
  .updatearea = esp_dsi_updatearea,
#endif
};

static int esp_dsi_wait_cmd_space(struct esp_dsi_s *priv)
{
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++)
    {
      if (!mipi_dsi_host_ll_gen_is_cmd_fifo_full(priv->hal.host))
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

static int esp_dsi_wait_payload_space(struct esp_dsi_s *priv)
{
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++)
    {
      if (!mipi_dsi_host_ll_gen_is_write_fifo_full(priv->hal.host))
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

static int esp_dsi_wait_tx_done(struct esp_dsi_s *priv)
{
  unsigned int n;

  for (n = 0; n < DSI_POLL_LOOPS; n++)
    {
      if (mipi_dsi_host_ll_gen_is_cmd_fifo_empty(priv->hal.host) &&
          mipi_dsi_host_ll_gen_is_write_fifo_empty(priv->hal.host))
        {
          return OK;
        }
    }

  syslog(LOG_ERR,
         "ERROR: MIPI command transmit timeout (status=%08lx int=%08lx)\n",
         (unsigned long)priv->hal.host->cmd_pkt_status.val,
         (unsigned long)priv->hal.host->int_st0.val);
  return -ETIMEDOUT;
}

static ssize_t esp_dsi_transfer(struct mipi_dsi_host *host,
                                const struct mipi_dsi_msg *msg)
{
  struct esp_dsi_s *priv = (struct esp_dsi_s *)host;
  const uint8_t *buf;
  uint32_t word;
  size_t left;
  uint16_t header = 0;
  int ret;

  if (host == NULL || msg == NULL || msg->channel > 3 ||
      msg->tx_len > UINT16_MAX ||
      (msg->tx_len != 0 && msg->tx_buf == NULL))
    {
      return -EINVAL;
    }

  buf = msg->tx_buf;

  if (msg->rx_len != 0)
    {
      return -ENOTSUP;
    }

  syslog(LOG_INFO, "MIPI: transfer locking host\n");
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "MIPI: transfer host locked\n");

  if (msg->tx_len > 2)
    {
      left = msg->tx_len;
      while (left != 0)
        {
          size_t ncopy = left > sizeof(word) ? sizeof(word) : left;
          word = 0;
          memcpy(&word, buf, ncopy);
          ret = esp_dsi_wait_payload_space(priv);
          if (ret < 0)
            {
              goto out;
            }

          mipi_dsi_host_ll_gen_write_payload_fifo(priv->hal.host, word);
          buf += ncopy;
          left -= ncopy;
        }

      header = msg->tx_len;
    }
  else if (msg->tx_len != 0)
    {
      header = buf[0];
      if (msg->tx_len == 2)
        {
          header |= (uint16_t)buf[1] << 8;
        }
    }

  ret = esp_dsi_wait_cmd_space(priv);
  if (ret >= 0)
    {
      syslog(LOG_INFO, "MIPI: writing packet header type=%02x hdr=%04x\n",
             msg->type, header);
      mipi_dsi_host_ll_gen_set_packet_header(priv->hal.host, msg->channel,
                                              msg->type, header >> 8,
                                              header & 0xff);
      ret = esp_dsi_wait_tx_done(priv);
      if (ret >= 0)
        {
          /* Keep vendor writes serialized beyond the FIFO boundary. */

          up_udelay(1000);
          ret = msg->tx_len;
        }
    }

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_dsi_attach(struct mipi_dsi_host *host,
                          struct mipi_dsi_device *device)
{
  (void)host;

  return device->lanes == DSI_LANES ? OK : -EINVAL;
}

static int esp_dsi_detach(struct mipi_dsi_host *host,
                          struct mipi_dsi_device *device)
{
  (void)host;
  (void)device;

  return OK;
}

static const struct mipi_dsi_host_ops g_host_ops =
{
  .attach = esp_dsi_attach,
  .detach = esp_dsi_detach,
  .transfer = esp_dsi_transfer,
};

static int esp_dsi_dcs(uint8_t cmd, const uint8_t *data, size_t len)
{
  uint8_t packet[8];
  struct mipi_dsi_msg msg;
  ssize_t ret;

  if (len + 1 > sizeof(packet))
    {
      return -E2BIG;
    }

  packet[0] = cmd;
  if (len != 0)
    {
      memcpy(&packet[1], data, len);
    }

  memset(&msg, 0, sizeof(msg));
  msg.channel = 0;
  msg.type = len == 0 ? MIPI_DSI_DT_DCS_SHORT_WRITE_0 :
             len == 1 ? MIPI_DSI_DT_DCS_SHORT_WRITE_1 :
                        MIPI_DSI_DT_DCS_LONG_WRITE;
  msg.tx_buf = packet;
  msg.tx_len = len + 1;
  syslog(LOG_INFO, "MIPI: DCS write cmd=%02x len=%u\n", cmd,
         (unsigned int)len);
  ret = mipi_dsi_transfer(g_dsi.panel, &msg);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "MIPI: DCS write cmd=%02x result=%ld\n", cmd, (long)ret);
  return ret < 0 ? -EIO : OK;
}

static int esp_dsi_dcs_write(uint8_t cmd, uint8_t data)
{
  return esp_dsi_dcs(cmd, &data, 1);
}

/* Single-shot DCS read with BTA and full RX cleanup.
 *
 * This reads one DCS response from the panel.  BTA is enabled only for the
 * duration of this call and disabled again on exit, so it does not interfere
 * with the normal LPDT command path used for panel initialization.
 *
 * Returns the number of bytes read (1-4) on success, or a negative errno.
 */

static int esp_dsi_read_dcs_cmd(struct esp_dsi_s *priv, uint8_t cmd,
                                uint8_t *buf, size_t max_len)
{
  unsigned int n;
  uint32_t val;
  int ret;

  if (priv == NULL || buf == NULL || max_len == 0)
    {
      return -EINVAL;
    }

  /* Drain any stale data in the RX FIFO before starting a new read. */

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host))
    {
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
  for (n = 0; n < DSI_POLL_LOOPS; n++)
    {
      if (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host))
        {
          val = mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
          ret = 1;
          break;
        }

      if (!mipi_dsi_host_ll_gen_is_read_cmd_busy(priv->hal.host) &&
          mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host))
        {
          /* Command completed but no data — panel did not respond. */

          ret = -ENODATA;
          break;
        }
    }

  /* Disable BTA immediately after the read. */

  mipi_dsi_host_ll_enable_bta(priv->hal.host, false);

  if (ret > 0)
    {
      /* The RX FIFO word contains up to 4 bytes in little-endian order. */

      size_t ncopy = max_len < 4 ? max_len : 4;
      memcpy(buf, &val, ncopy);
      syslog(LOG_INFO,
             "MIPI: DCS read cmd=%02x data=%02x %02x %02x %02x\n",
             cmd, buf[0],
             ncopy > 1 ? buf[1] : 0, ncopy > 2 ? buf[2] : 0,
             ncopy > 3 ? buf[3] : 0);
    }
  else
    {
      syslog(LOG_ERR,
             "MIPI: DCS read cmd=%02x failed ret=%d "
             "int_st0=%08lx int_st1=%08lx phy=%08lx\n",
             cmd, ret,
             (unsigned long)priv->hal.host->int_st0.val,
             (unsigned long)priv->hal.host->int_st1.val,
             (unsigned long)priv->hal.host->phy_status.val);
    }

  /* Drain the RX FIFO and clear any residual interrupt flags so they do not
   * affect the subsequent video mode operation.
   */

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host))
    {
      (void)mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
    }

  /* Write-1-to-clear on interrupt status registers. */

  priv->hal.host->int_st0.val = priv->hal.host->int_st0.val;
  priv->hal.host->int_st1.val = priv->hal.host->int_st1.val;

  return ret;
}

static int esp_dsi_dma_submit(struct esp_dsi_s *priv)
{
  dw_gdma_block_markers_t markers =
    {
      .is_valid = true,
      .is_last = true
    };

  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* A completed transfer disables the channel automatically.  Use the
   * normal disable path as a harmless guard before reconfiguration.  The
   * force-abort path can violate AXI protocol and has been observed to hang
   * the system when called on the display channel after frame completion.
   */

  dw_gdma_channel_enable_ctrl(priv->dma, false);
  ret = OK;
  dw_gdma_lli_set_block_markers(dw_gdma_link_list_get_item(priv->link, 0),
                                markers);
  if (dw_gdma_channel_use_link_list(priv->dma, priv->link) != ESP_OK ||
      dw_gdma_channel_enable_ctrl(priv->dma, true) != ESP_OK)
    {
      ret = -EIO;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_dsi_dma_initialize(struct esp_dsi_s *priv)
{
  dw_gdma_channel_alloc_config_t chan_cfg =
    {
      .src =
        {
          DW_GDMA_BLOCK_TRANSFER_LIST, DW_GDMA_ROLE_MEM,
          DW_GDMA_HANDSHAKE_HW, 5, 0
        },

      .dst =
        {
          DW_GDMA_BLOCK_TRANSFER_LIST, DW_GDMA_ROLE_PERIPH_DSI,
          DW_GDMA_HANDSHAKE_HW, 2, 0
        },

      .flow_controller = DW_GDMA_FLOW_CTRL_SELF,

      /* Keep display traffic below latency-sensitive CPU/UART traffic. */

      .chan_priority = 0
    };

  dw_gdma_link_list_config_t list_cfg =
    {
      .num_items = 1,
      .link_type = DW_GDMA_LINKED_LIST_TYPE_SINGLY
    };

  dw_gdma_block_transfer_config_t transfer =
    {
      .src =
        {
          (uint32_t)g_framebuffer, DW_GDMA_TRANS_WIDTH_64,
          DW_GDMA_BURST_MODE_INCREMENT, DW_GDMA_BURST_ITEMS_16, 16
        },

      .dst =
        {
          MIPI_DSI_BRG_MEM_BASE, DW_GDMA_TRANS_WIDTH_64,
          DW_GDMA_BURST_MODE_FIXED, DW_GDMA_BURST_ITEMS_16, 16
        },

      .size = LCD_FB_SIZE / 8
    };

  dw_gdma_block_markers_t markers =
    {
      .is_valid = true,
      .is_last = true
    };

  dw_gdma_lli_handle_t item;

  if (dw_gdma_new_channel(&chan_cfg, &priv->dma) != ESP_OK ||
      dw_gdma_new_link_list(&list_cfg, &priv->link) != ESP_OK)
    {
      return -ENOMEM;
    }

  item = dw_gdma_link_list_get_item(priv->link, 0);
  if (dw_gdma_lli_config_transfer(item, &transfer) != ESP_OK ||
      dw_gdma_lli_set_block_markers(item, markers) != ESP_OK ||
      dw_gdma_channel_use_link_list(priv->dma, priv->link) != ESP_OK)
    {
      return -EIO;
    }

  return OK;
}

static int esp_dsi_hardware_initialize(struct esp_dsi_s *priv)
{
  mipi_dsi_hal_config_t hal_cfg =
    {
      .bus_id = DSI_BUS,
      .lane_bit_rate_mbps = DSI_LANE_RATE_MBPS,
      .num_data_lanes = DSI_LANES
    };

  irqstate_t flags;
  uint8_t id[4] =
    {
      0
    };

  uint8_t dref;
  uint8_t mul;
  uint8_t pwr = 0;
  bool use_rail;
  uint32_t div;
  unsigned int n;
  int ret;

  syslog(LOG_INFO, "MIPI: enabling 2.5V PHY LDO\n");
  ldo_ll_voltage_to_dref_mul(LDO_ID2UNIT(3), 2500, &dref, &mul,
                             &use_rail);
  flags = enter_critical_section();
  ldo_ll_adjust_voltage(LDO_ID2UNIT(3), dref, mul, use_rail);
  ldo_ll_set_owner(LDO_ID2UNIT(3), LDO_LL_UNIT_OWNER_SW);
  ldo_ll_enable_ripple_suppression(LDO_ID2UNIT(3), true);
  ldo_ll_enable(LDO_ID2UNIT(3), true);
  leave_critical_section(flags);
  up_udelay(10000);
  syslog(LOG_INFO, "MIPI: 2.5V PHY LDO enabled\n");

  syslog(LOG_INFO, "MIPI: enabling DSI peripheral clocks\n");
  flags = up_irq_save();
  _clk_gate_ll_ref_20m_clk_en(true);
  _mipi_dsi_ll_enable_bus_clock(DSI_BUS, true);
  (mipi_dsi_ll_reset_register)(DSI_BUS);
  _mipi_dsi_ll_set_phy_config_clock_source(
    DSI_BUS, MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT);
  (mipi_dsi_ll_enable_phy_config_clock)(DSI_BUS, true);
  _mipi_dsi_ll_set_phy_pllref_clock_source(
    DSI_BUS, MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT);
  _mipi_dsi_ll_set_phy_pll_ref_clock_div(DSI_BUS, 1);
  (mipi_dsi_ll_enable_phy_pllref_clock)(DSI_BUS, true);
  up_irq_restore(flags);
  syslog(LOG_INFO, "MIPI: DSI and REF_20M clocks enabled\n");

  syslog(LOG_INFO, "MIPI: configuring DSI PHY and PLL\n");
  priv->hal.host = MIPI_DSI_LL_GET_HOST(hal_cfg.bus_id);
  priv->hal.bridge = MIPI_DSI_LL_GET_BRG(hal_cfg.bus_id);
  mipi_dsi_phy_ll_set_data_lane_number(priv->hal.host,
                                        hal_cfg.num_data_lanes);
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
                                  DSI_LANE_RATE_MBPS);
  syslog(LOG_INFO, "MIPI: DSI PHY PLL configured\n");

  syslog(LOG_INFO, "MIPI: waiting for PHY PLL lock (status=%08lx)\n",
         (unsigned long)priv->hal.host->phy_status.val);
  for (n = 0; n < 500; n++)
    {
      if (mipi_dsi_phy_ll_is_pll_locked(priv->hal.host))
        {
          break;
        }

      up_udelay(1000);
    }

  if (n == 500)
    {
      syslog(LOG_ERR, "ERROR: MIPI PHY PLL lock timeout (status=%08lx)\n",
             (unsigned long)priv->hal.host->phy_status.val);
      return -ETIMEDOUT;
    }

  syslog(LOG_INFO, "MIPI: PHY PLL locked\n");

  for (n = 0; n < 500; n++)
    {
      if (mipi_dsi_phy_ll_are_lanes_stopped(priv->hal.host, DSI_LANES))
        {
          break;
        }

      up_udelay(1000);
    }

  if (n == 500)
    {
      syslog(LOG_ERR, "ERROR: MIPI PHY lanes did not enter stop state "
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
   * HS packets, which empties the host FIFO but is not accepted by EK79007.
   */

  mipi_dsi_host_ll_enable_te_ack(priv->hal.host, false);
  mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
  mipi_dsi_host_ll_enable_bta(priv->hal.host, false);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    priv->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    priv->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    priv->hal.host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_long_wr_speed_mode(
    priv->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    priv->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    priv->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    priv->hal.host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(
    priv->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(
    priv->hal.host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(
    priv->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(
    priv->hal.host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_mrps_speed_mode(
    priv->hal.host, MIPI_DSI_LL_TRANS_SPEED_LP);
  syslog(LOG_INFO, "MIPI: DBI commands configured for LP mode\n");

  priv->host.bus = DSI_BUS;
  priv->host.ops = &g_host_ops;
  ret = mipi_dsi_host_register(&priv->host);
  if (ret < 0)
    {
      return ret;
    }

  priv->panel = mipi_dsi_device_register(&priv->host, "ek79007", 0);
  if (priv->panel == NULL)
    {
      return -ENOMEM;
    }

  priv->panel->lanes = DSI_LANES;
  priv->panel->format = MIPI_DSI_FMT_RGB888;
  priv->panel->mode_flags = MIPI_DSI_MODE_VIDEO |
                            MIPI_DSI_MODE_VIDEO_BURST;
  if (mipi_dsi_attach(priv->panel) < 0)
    {
      return -EIO;
    }

  syslog(LOG_INFO, "MIPI: initializing EK79007 panel\n");
  syslog(LOG_INFO, "MIPI: configuring EK79007 reset GPIO%d\n",
         BOARD_LCD_RST);
  esp_configgpio(BOARD_LCD_RST, OUTPUT);

  /* EK79007 requires the DSI lanes to remain in LP11 around GRB and needs
   * at least 55 ms after GRB is released before accepting initialization
   * commands.  Use conservative margins because the panel rails are supplied
   * independently from the ESP32-P4 DPHY LDO.
   */

  esp_gpiowrite(BOARD_LCD_RST, true);
  syslog(LOG_INFO, "MIPI: waiting for EK79007 power/reset stabilization\n");
  up_udelay(20000);
  syslog(LOG_INFO, "MIPI: driving EK79007 reset low\n");
  esp_gpiowrite(BOARD_LCD_RST, false);
  syslog(LOG_INFO, "MIPI: waiting after EK79007 reset assert\n");
  up_udelay(30000);
  syslog(LOG_INFO, "MIPI: driving EK79007 reset high\n");
  esp_gpiowrite(BOARD_LCD_RST, true);
  syslog(LOG_INFO, "MIPI: waiting after EK79007 reset release\n");
  up_udelay(120000);
  syslog(LOG_INFO, "MIPI: EK79007 reset complete\n");

  /* Prime the LP command path with a standard DCS NOP before accessing
   * vendor registers.  This is harmless to the panel state and makes the
   * first vendor write occur only after a completed LP transaction.
   */

  ret = esp_dsi_dcs(0x00, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(2000);
  ret = esp_dsi_dcs_write(0xb2, 0x10);
  if (ret < 0)
    {
      return ret;
    }

#if LCD_EK79007_BIST
  ret = esp_dsi_dcs_write(0xb1, 0x08);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "MIPI: EK79007 internal BIST enabled\n");
#endif

  ret = esp_dsi_dcs_write(0x80, 0x8b);
  ret |= esp_dsi_dcs_write(0x81, 0x78);
  ret |= esp_dsi_dcs_write(0x82, 0x84);
  ret |= esp_dsi_dcs_write(0x83, 0x88);
  ret |= esp_dsi_dcs_write(0x84, 0xa8);
  ret |= esp_dsi_dcs_write(0x85, 0xe3);
  ret |= esp_dsi_dcs_write(0x86, 0x88);
  ret |= esp_dsi_dcs_write(0x3a, 0x77);
  ret |= esp_dsi_dcs(0x11, NULL, 0);
  up_udelay(120000);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_dsi_dcs(0x29, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(20000);

  /* Diagnostic: read display ID and power mode to verify the panel is
   * actually responding to commands.  This is a single-shot read that
   * enables BTA only for the duration of the call and cleans up afterward.
   */

  ret = esp_dsi_read_dcs_cmd(priv, 0x04, id, sizeof(id));
  syslog(ret > 0 ? LOG_INFO : LOG_WARNING,
         "MIPI: Display ID read: ret=%d id=%02x %02x %02x %02x\n",
         ret, id[0], id[1], id[2], id[3]);

  ret = esp_dsi_read_dcs_cmd(priv, 0x0a, &pwr, 1);
  syslog(ret > 0 ? LOG_INFO : LOG_WARNING,
         "MIPI: Power mode read: ret=%d val=%02x\n", ret, pwr);

  priv->hal.expect_dpi_clock_freq_mhz = 48.0f;
  div = mipi_dsi_hal_host_dpi_calculate_divider(&priv->hal, 240.0f, 48.0f);
  flags = up_irq_save();
  _clk_gate_ll_ref_240m_clk_en(true);
  (mipi_dsi_ll_set_dpi_clock_source)(DSI_BUS,
                                     MIPI_DSI_DPI_CLK_SRC_DEFAULT);
  (mipi_dsi_ll_set_dpi_clock_div)(DSI_BUS, div);
  (mipi_dsi_ll_enable_dpi_clock)(DSI_BUS, true);
  up_irq_restore(flags);
  syslog(LOG_INFO, "MIPI: REF_240M and DPI clocks enabled (div=%lu)\n",
         (unsigned long)div);

  mipi_dsi_host_ll_dpi_set_vcid(priv->hal.host, 0);

  /* ESP32-P4 v3.x exposes separate requested and active video registers.
   * Disable shadow to write directly to active registers, avoiding
   * potential issues with shadow commit not completing before video start.
   */

  priv->hal.host->vid_shadow_ctrl.vid_shadow_en = 0;
  mipi_dsi_host_ll_dpi_set_color_coding(priv->hal.host,
                                         LCD_COLOR_FMT_RGB888, 0);
  mipi_dsi_host_ll_dpi_set_timing_polarity(priv->hal.host, false, false,
                                            false, false, false);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(priv->hal.host, true,
                                                    true);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(priv->hal.host, true, true,
                                                  true, true);
  mipi_dsi_host_ll_dpi_enable_lp_command(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_set_video_burst_type(
    priv->hal.host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);
  mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(priv->hal.host,
                                                   LCD_WIDTH);
  mipi_dsi_host_ll_dpi_set_trunks_num(priv->hal.host, 0);
  mipi_dsi_host_ll_dpi_set_null_packet_size(priv->hal.host, 0);
  mipi_dsi_hal_host_dpi_set_horizontal_timing(&priv->hal, LCD_HSYNC, LCD_HBP,
                                               LCD_WIDTH, LCD_HFP);
  mipi_dsi_hal_host_dpi_set_vertical_timing(&priv->hal, LCD_VSYNC, LCD_VBP,
                                             LCD_HEIGHT, LCD_VFP);
  mipi_dsi_brg_ll_set_num_pixel_bits(priv->hal.bridge,
                                      LCD_WIDTH * LCD_HEIGHT * LCD_BPP);
  mipi_dsi_brg_ll_set_underrun_discard_count(priv->hal.bridge, LCD_WIDTH);
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

  syslog(LOG_INFO, "MIPI: configuring DPI bridge and display DMA\n");
  ret = esp_dsi_dma_initialize(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Diagnostic mode: use the DSI host's built-in pattern generator to
   * validate the panel, PHY and video timing independently of DW-GDMA.
   */

  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);

  /* Do not make video progress depend on a per-frame response.  Force CLK
   * lane to stay in HS mode and disable LP blanking to rule out LP/HS
   * transition issues that may prevent the panel from receiving video data.
   */

  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(priv->hal.host,
                                                    false, false);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(priv->hal.host,
                                                  false, false, false,
                                                  false);
  mipi_dsi_host_ll_dpi_enable_lp_command(priv->hal.host, false);
  mipi_dsi_host_ll_set_clock_lane_state(
    priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
  syslog(LOG_INFO, "MIPI: forced HS video mode (no LP blanking)\n");
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                         MIPI_DSI_PATTERN_BAR_VERTICAL);
  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
  up_udelay(100000);
  syslog(LOG_INFO,
         "MIPI: status shadow=%08lx mode=%08lx active=%08lx "
         "pkt=%08lx/%08lx phy=%08lx int=%08lx/%08lx\n",
         (unsigned long)priv->hal.host->vid_shadow_ctrl.val,
         (unsigned long)priv->hal.host->vid_mode_cfg.val,
         (unsigned long)priv->hal.host->vid_mode_cfg_act.val,
         (unsigned long)priv->hal.host->vid_pkt_size.val,
         (unsigned long)priv->hal.host->vid_pkt_size_act.val,
         (unsigned long)priv->hal.host->phy_status.val,
         (unsigned long)priv->hal.host->int_st0.val,
         (unsigned long)priv->hal.host->int_st1.val);
  syslog(LOG_INFO,
         "MIPI: bridge en=%08lx dpi=%08lx fifo=%08lx raw=%08lx\n",
         (unsigned long)priv->hal.bridge->en.val,
         (unsigned long)priv->hal.bridge->dpi_lcd_ctl.val,
         (unsigned long)priv->hal.bridge->fifo_flow_status.val,
         (unsigned long)priv->hal.bridge->int_raw.val);
  syslog(LOG_INFO,
         "MIPI: H req=%lu/%lu/%lu act=%lu/%lu/%lu\n",
         (unsigned long)priv->hal.host->vid_hsa_time.val,
         (unsigned long)priv->hal.host->vid_hbp_time.val,
         (unsigned long)priv->hal.host->vid_hline_time.val,
         (unsigned long)priv->hal.host->vid_hsa_time_act.val,
         (unsigned long)priv->hal.host->vid_hbp_time_act.val,
         (unsigned long)priv->hal.host->vid_hline_time_act.val);
  syslog(LOG_INFO,
         "MIPI: V req=%lu/%lu/%lu/%lu act=%lu/%lu/%lu/%lu "
         "color=%08lx/%08lx\n",
         (unsigned long)priv->hal.host->vid_vsa_lines.val,
         (unsigned long)priv->hal.host->vid_vbp_lines.val,
         (unsigned long)priv->hal.host->vid_vfp_lines.val,
         (unsigned long)priv->hal.host->vid_vactive_lines.val,
         (unsigned long)priv->hal.host->vid_vsa_lines_act.val,
         (unsigned long)priv->hal.host->vid_vbp_lines_act.val,
         (unsigned long)priv->hal.host->vid_vfp_lines_act.val,
         (unsigned long)priv->hal.host->vid_vactive_lines_act.val,
         (unsigned long)priv->hal.host->dpi_color_coding.val,
         (unsigned long)priv->hal.host->dpi_color_coding_act.val);

  esp_configgpio(BOARD_LCD_BACKLIGHT, OUTPUT);
  esp_gpiowrite(BOARD_LCD_BACKLIGHT, true);
  syslog(LOG_INFO, "MIPI: vertical color-bar test pattern enabled\n");
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (!g_dsi.initialized)
    {
      memset(g_framebuffer, 0, sizeof(g_framebuffer));
      ret = esp_dsi_hardware_initialize(&g_dsi);
      if (ret >= 0)
        {
          g_dsi.initialized = true;
        }
    }
  else
    {
      ret = OK;
    }

  return ret;
}

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  return display == 0 && vplane == 0 ? &g_fbops : NULL;
}

void up_fbuninitialize(int display)
{
  if (display == 0)
    {
      esp_gpiowrite(BOARD_LCD_BACKLIGHT, false);
    }
}
