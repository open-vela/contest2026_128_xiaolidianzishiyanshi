/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_mipi_csi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <sys/time.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/wqueue.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_private/esp_psram_extram.h"
#include "esp_private/periph_ctrl.h"
#include "espressif/esp_irq.h"
#include "hal/clk_gate_ll.h"
#include "hal/dw_gdma_hal.h"
#include "hal/dw_gdma_ll.h"
#include "hal/ldo_ll.h"
#include "hal/mipi_csi_brg_ll.h"
#include "hal/mipi_csi_hal.h"
#include "hal/mipi_csi_ll.h"

#include "esp_mipi_csi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CSI_BUS                    0
#define CSI_DMA_CHANNEL            1
#define CSI_RAW10_DATA_TYPE        0x2b
#define CSI_DMA_ALIGNMENT          64
#define CSI_DMA_ERROR_EVENTS                                           \
  (DW_GDMA_LL_CHANNEL_EVENT_SRC_DEC_ERR |                              \
   DW_GDMA_LL_CHANNEL_EVENT_DST_DEC_ERR |                              \
   DW_GDMA_LL_CHANNEL_EVENT_SRC_SLV_ERR |                              \
   DW_GDMA_LL_CHANNEL_EVENT_DST_SLV_ERR |                              \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_DEC_ERR |                           \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_DEC_ERR |                           \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_SLV_ERR |                           \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_SLV_ERR |                           \
   DW_GDMA_LL_CHANNEL_EVENT_SHADOWREG_OR_LLI_INVALID_ERR)
#define CSI_DMA_EVENTS                                                 \
  (DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE | CSI_DMA_ERROR_EVENTS)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_csi_s
{
  struct imgdata_s data;
  mipi_csi_hal_context_t hal;
  dw_gdma_hal_context_t dma;
  mutex_t lock;
  struct work_s work;
  FAR uint8_t *buffer;
  uint32_t buffer_size;
  imgdata_capture_t callback;
  FAR void *callback_arg;
  struct timeval timestamp;
  volatile uint32_t dma_status;
  int dma_cpuint;
  bool initialized;
  bool capturing;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_csi_init(FAR struct imgdata_s *data);
static int esp_csi_uninit(FAR struct imgdata_s *data);
static int esp_csi_set_buf(FAR struct imgdata_s *data, uint8_t nr_datafmts,
                           FAR imgdata_format_t *datafmts,
                           FAR uint8_t *addr, uint32_t size);
static int esp_csi_validate(FAR struct imgdata_s *data,
                            uint8_t nr_datafmts,
                            FAR imgdata_format_t *datafmts,
                            FAR imgdata_interval_t *interval);
static int esp_csi_start(FAR struct imgdata_s *data, uint8_t nr_datafmts,
                         FAR imgdata_format_t *datafmts,
                         FAR imgdata_interval_t *interval,
                         imgdata_capture_t callback, FAR void *arg);
static int esp_csi_stop(FAR struct imgdata_s *data);
static FAR void *esp_csi_alloc(FAR struct imgdata_s *data,
                               uint32_t align_size, uint32_t size);
static void esp_csi_free(FAR struct imgdata_s *data, FAR void *addr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_csi_ops =
{
  .init                   = esp_csi_init,
  .uninit                 = esp_csi_uninit,
  .set_buf                = esp_csi_set_buf,
  .validate_frame_setting = esp_csi_validate,
  .start_capture          = esp_csi_start,
  .stop_capture           = esp_csi_stop,
  .alloc                  = esp_csi_alloc,
  .free                   = esp_csi_free,
};

static struct esp_csi_s g_csi =
{
  .data =
  {
    .ops = &g_csi_ops,
  },
  .lock = NXMUTEX_INITIALIZER,
  .dma_cpuint = -1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool esp_csi_format_valid(uint8_t nr_datafmts,
                                 FAR imgdata_format_t *datafmts,
                                 FAR imgdata_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL || interval == NULL)
    {
      return false;
    }

  return datafmts[IMGDATA_FMT_MAIN].width == ESP_MIPI_CSI_WIDTH &&
         datafmts[IMGDATA_FMT_MAIN].height == ESP_MIPI_CSI_HEIGHT &&
         datafmts[IMGDATA_FMT_MAIN].pixelformat ==
           ESP_MIPI_CSI_PIX_FMT_RAW10 &&
         interval->numerator == ESP_MIPI_CSI_FRAME_INTERVAL_NUM &&
         interval->denominator == ESP_MIPI_CSI_FRAME_INTERVAL_DEN;
}

static void esp_csi_dma_disable(FAR struct esp_csi_s *priv)
{
  if (priv->dma.dev != NULL)
    {
      mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
      dw_gdma_ll_channel_enable_intr_propagation(priv->dma.dev,
                                                 CSI_DMA_CHANNEL,
                                                 CSI_DMA_EVENTS, false);
      dw_gdma_ll_channel_enable(priv->dma.dev, CSI_DMA_CHANNEL, false);
      dw_gdma_ll_channel_clear_intr(priv->dma.dev, CSI_DMA_CHANNEL,
                                    UINT32_MAX);
    }
}

static int esp_csi_dma_arm(FAR struct esp_csi_s *priv)
{
  dw_gdma_dev_t *dma = priv->dma.dev;
  int ret;

  ret = esp_cache_msync(priv->buffer, ESP_MIPI_CSI_FRAME_SIZE,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret != ESP_OK)
    {
      return -EIO;
    }

  dw_gdma_ll_channel_set_src_addr(dma, CSI_DMA_CHANNEL,
                                  MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_channel_set_dst_addr(dma, CSI_DMA_CHANNEL,
                                  (uint32_t)priv->buffer);
  dw_gdma_ll_channel_set_trans_block_size(dma, CSI_DMA_CHANNEL,
                                          ESP_MIPI_CSI_FRAME_SIZE / 8);
  dw_gdma_ll_channel_set_src_master_port(dma, CSI_DMA_CHANNEL,
                                         MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_channel_set_dst_master_port(dma, CSI_DMA_CHANNEL,
                                         (intptr_t)priv->buffer);
  dw_gdma_ll_channel_set_src_trans_width(dma, CSI_DMA_CHANNEL,
                                         DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_channel_set_dst_trans_width(dma, CSI_DMA_CHANNEL,
                                         DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_channel_set_src_burst_items(dma, CSI_DMA_CHANNEL,
                                         DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_channel_set_dst_burst_items(dma, CSI_DMA_CHANNEL,
                                         DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_channel_set_src_burst_mode(dma, CSI_DMA_CHANNEL,
                                        DW_GDMA_BURST_MODE_FIXED);
  dw_gdma_ll_channel_set_dst_burst_mode(dma, CSI_DMA_CHANNEL,
                                        DW_GDMA_BURST_MODE_INCREMENT);
  dw_gdma_ll_channel_set_src_burst_len(dma, CSI_DMA_CHANNEL, 16);
  dw_gdma_ll_channel_set_dst_burst_len(dma, CSI_DMA_CHANNEL, 16);
  dw_gdma_ll_channel_enable_src_periph_status_write_back(
    dma, CSI_DMA_CHANNEL, false);
  dw_gdma_ll_channel_enable_dst_periph_status_write_back(
    dma, CSI_DMA_CHANNEL, false);
  dw_gdma_ll_channel_set_block_markers(dma, CSI_DMA_CHANNEL,
                                       false, true, true);
  dw_gdma_ll_channel_clear_intr(dma, CSI_DMA_CHANNEL, UINT32_MAX);
  dw_gdma_ll_channel_enable_intr_propagation(dma, CSI_DMA_CHANNEL,
                                             CSI_DMA_EVENTS, true);
  dw_gdma_ll_channel_enable(dma, CSI_DMA_CHANNEL, true);
  mipi_csi_brg_ll_enable(priv->hal.bridge_dev, true);
  return OK;
}

static void esp_csi_complete_worker(FAR void *arg)
{
  FAR struct esp_csi_s *priv = arg;
  imgdata_capture_t callback;
  FAR void *callback_arg;
  uint8_t result;

  nxmutex_lock(&priv->lock);
  callback = priv->callback;
  callback_arg = priv->callback_arg;
  result = (priv->dma_status & CSI_DMA_ERROR_EVENTS) == 0 ? 0 : EIO;
  priv->callback = NULL;
  priv->callback_arg = NULL;
  nxmutex_unlock(&priv->lock);

  if (callback != NULL)
    {
      if (result == 0 &&
          esp_cache_msync(priv->buffer, ESP_MIPI_CSI_FRAME_SIZE,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED) != ESP_OK)
        {
          result = EIO;
        }

      callback(result, result == 0 ? ESP_MIPI_CSI_FRAME_SIZE : 0,
               &priv->timestamp, callback_arg);
    }
}

int IRAM_ATTR esp_mipi_csi_dma_interrupt(int irq, FAR void *context,
                                         FAR void *arg)
{
  FAR struct esp_csi_s *priv = &g_csi;
  uint32_t status;

  (void)irq;
  (void)context;
  (void)arg;

  if (priv->dma.dev == NULL)
    {
      return OK;
    }

  status = dw_gdma_ll_channel_get_intr_status(priv->dma.dev,
                                               CSI_DMA_CHANNEL);
  dw_gdma_ll_channel_clear_intr(priv->dma.dev, CSI_DMA_CHANNEL, status);
  if ((status & CSI_DMA_EVENTS) == 0 || !priv->capturing)
    {
      return OK;
    }

  mipi_csi_brg_ll_enable(priv->hal.bridge_dev, false);
  dw_gdma_ll_channel_enable_intr_propagation(priv->dma.dev,
                                             CSI_DMA_CHANNEL,
                                             CSI_DMA_EVENTS, false);
  priv->dma_status = status;
  priv->capturing = false;
  gettimeofday(&priv->timestamp, NULL);
  (void)work_queue(LPWORK, &priv->work, esp_csi_complete_worker, priv, 0);
  return OK;
}

static int esp_csi_hardware_initialize(FAR struct esp_csi_s *priv)
{
  /* Keep the parameter order used by ESP-IDF.  The current HAL names are
   * transposed relative to the bridge register fields.
   */

  mipi_csi_hal_config_t config =
    {
      .lanes_num          = ESP_MIPI_CSI_LANES,
      .frame_width        = ESP_MIPI_CSI_HEIGHT,
      .frame_height       = ESP_MIPI_CSI_WIDTH,
      .in_bpp             = ESP_MIPI_CSI_BITS_PER_PIXEL,
      .out_bpp            = ESP_MIPI_CSI_BITS_PER_PIXEL,
      .byte_swap_en       = false,
      .lane_bit_rate_mbps = ESP_MIPI_CSI_LANE_RATE_MBPS,
    };

  irqstate_t flags;
  uint8_t dref;
  uint8_t mul;
  bool use_rail;
#ifndef CONFIG_ESPRESSIF_MIPI_DSI
  int cpuint;
#endif

  ldo_ll_voltage_to_dref_mul(LDO_ID2UNIT(3), 2500, &dref, &mul, &use_rail);
  flags = enter_critical_section();
  ldo_ll_adjust_voltage(LDO_ID2UNIT(3), dref, mul, use_rail);
  ldo_ll_set_owner(LDO_ID2UNIT(3), LDO_LL_UNIT_OWNER_SW);
  ldo_ll_enable_ripple_suppression(LDO_ID2UNIT(3), true);
  ldo_ll_enable(LDO_ID2UNIT(3), true);
  leave_critical_section(flags);
  up_udelay(10000);

  PERIPH_RCC_ATOMIC()
    {
      clk_gate_ll_ref_20m_clk_en(true);
      /* The host and bridge have independent gates.  The Espressif CSI
       * controller claims and clocks the bridge before touching either MMIO
       * block.  Without this sequence, the first CSI host register access can
       * raise a load-access fault on ESP32-P4.
       */

      mipi_csi_ll_enable_brg_module_clock(CSI_BUS, true);
      mipi_csi_ll_reset_brg_module_clock(CSI_BUS);
      mipi_csi_brg_ll_enable_clock(
        MIPI_CSI_BRG_LL_GET_HW(CSI_BUS), true);
      mipi_csi_ll_enable_host_bus_clock(CSI_BUS, false);
      mipi_csi_ll_enable_host_bus_clock(CSI_BUS, true);
      mipi_csi_ll_reset_host_clock(CSI_BUS);
      mipi_csi_ll_set_phy_clock_source(CSI_BUS,
                                       MIPI_CSI_PHY_CLK_SRC_DEFAULT);
      mipi_csi_ll_enable_phy_config_clock(CSI_BUS, false);
      mipi_csi_ll_enable_phy_config_clock(CSI_BUS, true);
      dw_gdma_ll_enable_bus_clock(0, true);
    }

  mipi_csi_hal_init(&priv->hal, &config);
  mipi_csi_brg_ll_set_burst_len(priv->hal.bridge_dev, 512);
  mipi_csi_brg_ll_set_data_type_min(priv->hal.bridge_dev,
                                    CSI_RAW10_DATA_TYPE);
  mipi_csi_brg_ll_set_data_type_max(priv->hal.bridge_dev,
                                    CSI_RAW10_DATA_TYPE);
  mipi_csi_brg_ll_enable_color_conversion(priv->hal.bridge_dev, true);
  mipi_csi_brg_ll_set_color_mode_bypass(priv->hal.bridge_dev, true);

  /* Do not reset the global DW-GDMA block here: other peripherals may own
   * another channel.  CSI owns channel 1 only.
   */

  priv->dma.dev = DW_GDMA_LL_GET_HW(0);
  dw_gdma_ll_enable_controller(priv->dma.dev, true);
  dw_gdma_ll_enable_intr_global(priv->dma.dev, true);
  dw_gdma_ll_channel_set_trans_flow(priv->dma.dev, CSI_DMA_CHANNEL,
                                    DW_GDMA_ROLE_PERIPH_CSI,
                                    DW_GDMA_ROLE_MEM,
                                    DW_GDMA_FLOW_CTRL_SRC);
  dw_gdma_ll_channel_set_src_multi_block_type(
    priv->dma.dev, CSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS);
  dw_gdma_ll_channel_set_dst_multi_block_type(priv->dma.dev,
    CSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS);
  dw_gdma_ll_channel_set_src_handshake_interface(
    priv->dma.dev, CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_interface(
    priv->dma.dev, CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_src_handshake_periph(
    priv->dma.dev, CSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_CSI);
  dw_gdma_ll_channel_set_priority(priv->dma.dev, CSI_DMA_CHANNEL, 1);
  dw_gdma_ll_channel_set_src_outstanding_limit(priv->dma.dev,
                                                CSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_dst_outstanding_limit(priv->dma.dev,
                                                CSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_src_periph_status_addr(
    priv->dma.dev, CSI_DMA_CHANNEL, MIPI_CSI_BRG_MEM_BASE);
  dw_gdma_ll_channel_set_dst_periph_status_addr(priv->dma.dev,
                                                 CSI_DMA_CHANNEL, 0);
  dw_gdma_ll_channel_enable_intr_generation(priv->dma.dev,
                                             CSI_DMA_CHANNEL,
                                             UINT32_MAX, true);
  dw_gdma_ll_channel_clear_intr(priv->dma.dev, CSI_DMA_CHANNEL,
                                UINT32_MAX);

#ifndef CONFIG_ESPRESSIF_MIPI_DSI
  /* With no display driver, CSI owns the DW-GDMA interrupt.  When DSI is
   * enabled it already owns this SoC-wide source and dispatches channel 1 to
   * esp_mipi_csi_dma_interrupt().
   */

  sched_lock();
  cpuint = esp_setup_irq(DW_GDMA_INTR_SOURCE, ESP_IRQ_PRIORITY_DEFAULT,
                         ESP_IRQ_TRIGGER_LEVEL,
                         esp_mipi_csi_dma_interrupt, NULL);
  if (cpuint >= 0)
    {
      priv->dma_cpuint = cpuint;
      up_enable_irq(ESP_IRQ_DW_GDMA);
    }

  sched_unlock();
  if (cpuint < 0)
    {
      return cpuint;
    }
#endif

  return OK;
}

static int esp_csi_init(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_s *priv = (FAR struct esp_csi_s *)data;
  int ret;

  nxmutex_lock(&priv->lock);
  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  ret = esp_csi_hardware_initialize(priv);
  if (ret >= 0)
    {
      priv->initialized = true;
      syslog(LOG_INFO, "CSI: initialized 2-lane 405Mbps packed BGGR "
               "RAW10 transport\n");
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_csi_uninit(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_s *priv = (FAR struct esp_csi_s *)data;

  nxmutex_lock(&priv->lock);
  if (!priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  esp_csi_dma_disable(priv);
  priv->capturing = false;
  nxmutex_unlock(&priv->lock);

  work_cancel_sync(LPWORK, &priv->work);

  nxmutex_lock(&priv->lock);
  priv->callback = NULL;
  priv->callback_arg = NULL;
  priv->buffer = NULL;
  priv->buffer_size = 0;
  if (priv->dma_cpuint >= 0)
    {
      up_disable_irq(ESP_IRQ_DW_GDMA);
      esp_teardown_irq(DW_GDMA_INTR_SOURCE, priv->dma_cpuint);
      priv->dma_cpuint = -1;
    }

  PERIPH_RCC_ATOMIC()
    {
      mipi_csi_brg_ll_enable_clock(
        MIPI_CSI_BRG_LL_GET_HW(CSI_BUS), false);
      mipi_csi_ll_enable_brg_module_clock(CSI_BUS, false);
      mipi_csi_ll_enable_phy_config_clock(CSI_BUS, false);
      mipi_csi_ll_enable_host_bus_clock(CSI_BUS, false);
    }

  priv->initialized = false;
  nxmutex_unlock(&priv->lock);
  return OK;
}

static int esp_csi_set_buf(FAR struct imgdata_s *data,
                           uint8_t nr_datafmts,
                           FAR imgdata_format_t *datafmts,
                           FAR uint8_t *addr, uint32_t size)
{
  FAR struct esp_csi_s *priv = (FAR struct esp_csi_s *)data;
  imgdata_interval_t interval =
    {
      .numerator = ESP_MIPI_CSI_FRAME_INTERVAL_NUM,
      .denominator = ESP_MIPI_CSI_FRAME_INTERVAL_DEN,
    };

  int ret = OK;

  if (!esp_csi_format_valid(nr_datafmts, datafmts, &interval) ||
      addr == NULL || size < ESP_MIPI_CSI_FRAME_SIZE ||
      ((uintptr_t)addr & (CSI_DMA_ALIGNMENT - 1)) != 0 ||
      (ESP_MIPI_CSI_FRAME_SIZE & 7) != 0 ||
      !esp_psram_check_ptr_addr(addr) ||
      !esp_psram_check_ptr_addr(addr + ESP_MIPI_CSI_FRAME_SIZE - 1))
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);
  if (!priv->initialized)
    {
      ret = -ENODEV;
    }
  else if (priv->capturing)
    {
      ret = -EBUSY;
    }
  else
    {
      priv->buffer = addr;
      priv->buffer_size = size;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_csi_validate(FAR struct imgdata_s *data,
                            uint8_t nr_datafmts,
                            FAR imgdata_format_t *datafmts,
                            FAR imgdata_interval_t *interval)
{
  (void)data;
  return esp_csi_format_valid(nr_datafmts, datafmts, interval) ? OK :
         -ENOTSUP;
}

static int esp_csi_start(FAR struct imgdata_s *data,
                         uint8_t nr_datafmts,
                         FAR imgdata_format_t *datafmts,
                         FAR imgdata_interval_t *interval,
                         imgdata_capture_t callback, FAR void *arg)
{
  FAR struct esp_csi_s *priv = (FAR struct esp_csi_s *)data;
  int ret;

  if (!esp_csi_format_valid(nr_datafmts, datafmts, interval) ||
      callback == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);
  if (!priv->initialized || priv->buffer == NULL)
    {
      ret = -ENODEV;
    }
  else if (priv->capturing)
    {
      ret = -EBUSY;
    }
  else
    {
      priv->callback = callback;
      priv->callback_arg = arg;
      priv->dma_status = 0;
      priv->capturing = true;
      ret = esp_csi_dma_arm(priv);
      if (ret < 0)
        {
          priv->capturing = false;
          priv->callback = NULL;
          priv->callback_arg = NULL;
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_csi_stop(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_s *priv = (FAR struct esp_csi_s *)data;

  nxmutex_lock(&priv->lock);
  esp_csi_dma_disable(priv);
  priv->capturing = false;
  nxmutex_unlock(&priv->lock);

  work_cancel_sync(LPWORK, &priv->work);

  nxmutex_lock(&priv->lock);
  priv->callback = NULL;
  priv->callback_arg = NULL;
  nxmutex_unlock(&priv->lock);
  return OK;
}

static FAR void *esp_csi_alloc(FAR struct imgdata_s *data,
                               uint32_t align_size, uint32_t size)
{
  (void)data;

  if (align_size < CSI_DMA_ALIGNMENT)
    {
      align_size = CSI_DMA_ALIGNMENT;
    }

  return kmm_memalign(align_size, size);
}

static void esp_csi_free(FAR struct imgdata_s *data, FAR void *addr)
{
  (void)data;
  kmm_free(addr);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct imgdata_s *esp_mipi_csi_initialize(void)
{
  return &g_csi.data;
}
