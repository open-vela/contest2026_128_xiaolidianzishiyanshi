/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa_dma2d.c
 *
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * The descriptor layout and programming order are adapted from Espressif's
 * upper_hal_ppa and upper_hal_dma drivers.  FreeRTOS scheduling and channel
 * allocation are replaced with one NuttX mutex, fixed rev0/1.x-safe channels,
 * an RX EOF interrupt, and a bounded semaphore wait.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "esp32p4_ppa_internal.h"
#include "espressif/esp_irq.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_private/periph_ctrl.h"
#include "hal/dma2d_hal.h"
#include "hal/dma2d_ll.h"
#include "hal/dma2d_periph.h"
#include "hal/ppa_hal.h"
#include "hal/ppa_ll.h"
#include "soc/dma2d_channel.h"

#define ESP32P4_PPA_DMA_GROUP       0
#define ESP32P4_PPA_TX_BG_CHANNEL   0
#define ESP32P4_PPA_TX_FG_CHANNEL   1
#define ESP32P4_PPA_RX_CHANNEL      0
#define ESP32P4_PPA_RX_EVENTS       (DMA2D_LL_EVENT_RX_SUC_EOF | \
                                     DMA2D_LL_EVENT_RX_ERR_EOF | \
                                     DMA2D_LL_EVENT_RX_DESC_ERROR)
#define ESP32P4_PPA_RX_ERROR_EVENTS (ESP32P4_PPA_RX_EVENTS & \
                                     ~DMA2D_LL_EVENT_RX_SUC_EOF)
#define ESP32P4_PPA_CACHE_LINE_SIZE CONFIG_ESPRESSIF_CACHE_L1_CACHE_LINE_SIZE

struct esp32p4_ppa_dma_desc_s
{
  dma2d_descriptor_t desc;
  uint8_t padding[ESP32P4_PPA_CACHE_LINE_SIZE -
                  sizeof(dma2d_descriptor_t)];
} __attribute__((aligned(ESP32P4_PPA_CACHE_LINE_SIZE)));

struct esp32p4_ppa_platform_s
{
  mutex_t lock;
  sem_t done;
  ppa_hal_context_t ppa_hal;
  dma2d_hal_context_t dma_hal;
  unsigned int users;
  int cpuint;
  volatile uint32_t irq_status;
  bool initialized;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  uint32_t sequence_counter;
  volatile uint32_t active_sequence;
  volatile uint32_t isr_sequence;
  volatile uint32_t isr_status;
  volatile uint32_t isr_eof_desc;
  volatile uint32_t isr_ppa_int_raw;
  volatile uint32_t isr_blend_mode;
  volatile bool isr_rx_idle;
  volatile bool isr_desc_idle;
#endif
};

static struct esp32p4_ppa_dma_desc_s g_ppa_rx_desc;
static struct esp32p4_ppa_dma_desc_s g_ppa_tx_bg_desc;
static struct esp32p4_ppa_dma_desc_s g_ppa_tx_fg_desc;

static struct esp32p4_ppa_platform_s g_ppa =
{
  .lock = NXMUTEX_INITIALIZER,
  .done = SEM_INITIALIZER(0),
  .cpuint = -1,
};

static dma2d_data_burst_length_t
esp32p4_ppa_dma_burst(enum esp32p4_ppa_burst_length_e burst)
{
  return (dma2d_data_burst_length_t)
         (burst == ESP32P4_PPA_BURST_LENGTH_8 ? DMA2D_DATA_BURST_LENGTH_8 :
          burst == ESP32P4_PPA_BURST_LENGTH_16 ? DMA2D_DATA_BURST_LENGTH_16 :
          burst == ESP32P4_PPA_BURST_LENGTH_32 ? DMA2D_DATA_BURST_LENGTH_32 :
          burst == ESP32P4_PPA_BURST_LENGTH_64 ? DMA2D_DATA_BURST_LENGTH_64 :
                                                DMA2D_DATA_BURST_LENGTH_128);
}

static uint32_t esp32p4_ppa_pbyte(enum esp32p4_ppa_color_mode_e mode)
{
  switch (mode)
    {
      case ESP32P4_PPA_COLOR_MODE_RGB565:
        return DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
      case ESP32P4_PPA_COLOR_MODE_RGB888:
        return DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
      case ESP32P4_PPA_COLOR_MODE_A8:
        return DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
      default:
        return DMA2D_DESCRIPTOR_PBYTE_4B0_PER_PIXEL;
    }
}

static void esp32p4_ppa_drain_done(void)
{
  while (nxsem_trywait(&g_ppa.done) == 0)
    {
    }
}

static void esp32p4_ppa_rx_reset(void)
{
  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               UINT32_MAX, false);
  dma2d_ll_rx_clear_interrupt_status(g_ppa.dma_hal.dev,
                                     ESP32P4_PPA_RX_CHANNEL,
                                     UINT32_MAX);
  dma2d_ll_rx_stop(g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL);
  dma2d_hal_rx_reset_channel(&g_ppa.dma_hal, ESP32P4_PPA_RX_CHANNEL);
  ppa_ll_blend_reset(g_ppa.ppa_hal.dev);
  esp32p4_ppa_drain_done();
}

static void esp32p4_ppa_tx_reset(uint32_t channel)
{
  dma2d_ll_tx_enable_interrupt(g_ppa.dma_hal.dev, channel,
                               UINT32_MAX, false);
  dma2d_ll_tx_clear_interrupt_status(g_ppa.dma_hal.dev, channel,
                                     UINT32_MAX);
  dma2d_ll_tx_stop(g_ppa.dma_hal.dev, channel);
  dma2d_hal_tx_reset_channel(&g_ppa.dma_hal, channel);
}

static void esp32p4_ppa_configure_tx(uint32_t channel,
                                     dma2d_trigger_peripheral_t periph,
                                     int periph_id, bool descriptor_port,
                                     dma2d_data_burst_length_t burst)
{
  esp32p4_ppa_tx_reset(channel);
  dma2d_ll_tx_connect_to_periph(g_ppa.dma_hal.dev, channel,
                                periph, periph_id);
  dma2d_ll_tx_enable_reorder(g_ppa.dma_hal.dev, channel, false);
  dma2d_ll_tx_enable_dscr_port(g_ppa.dma_hal.dev, channel,
                               descriptor_port);
  dma2d_ll_tx_enable_owner_check(g_ppa.dma_hal.dev, channel, false);
  dma2d_ll_tx_enable_auto_write_back(g_ppa.dma_hal.dev, channel, false);
  dma2d_ll_tx_enable_eof_mode(g_ppa.dma_hal.dev, channel, true);
  dma2d_ll_tx_enable_descriptor_burst(g_ppa.dma_hal.dev, channel, true);
  dma2d_ll_tx_set_data_burst_length(g_ppa.dma_hal.dev, channel, burst);
  dma2d_ll_tx_enable_page_bound_wrap(g_ppa.dma_hal.dev, channel, true);
  dma2d_ll_tx_set_macro_block_size(g_ppa.dma_hal.dev, channel,
                                   DMA2D_MACRO_BLOCK_SIZE_NONE);
}

static void esp32p4_ppa_configure_rx(dma2d_trigger_peripheral_t periph,
                                     int periph_id, bool descriptor_port,
                                     dma2d_data_burst_length_t burst)
{
  dma2d_ll_rx_connect_to_periph(g_ppa.dma_hal.dev,
                                ESP32P4_PPA_RX_CHANNEL,
                                periph, periph_id);
  dma2d_ll_rx_enable_reorder(g_ppa.dma_hal.dev,
                             ESP32P4_PPA_RX_CHANNEL, false);
  dma2d_ll_rx_enable_dscr_port(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               descriptor_port);
  dma2d_ll_rx_enable_owner_check(g_ppa.dma_hal.dev,
                                 ESP32P4_PPA_RX_CHANNEL, false);
  dma2d_ll_rx_set_auto_return_owner(g_ppa.dma_hal.dev,
                                    ESP32P4_PPA_RX_CHANNEL,
                                    DMA2D_DESCRIPTOR_BUFFER_OWNER_CPU);
  dma2d_ll_rx_enable_descriptor_burst(g_ppa.dma_hal.dev,
                                      ESP32P4_PPA_RX_CHANNEL, true);
  dma2d_ll_rx_set_data_burst_length(g_ppa.dma_hal.dev,
                                    ESP32P4_PPA_RX_CHANNEL, burst);
  dma2d_ll_rx_enable_page_bound_wrap(g_ppa.dma_hal.dev,
                                     ESP32P4_PPA_RX_CHANNEL, true);
  dma2d_ll_rx_set_macro_block_size(g_ppa.dma_hal.dev,
                                   ESP32P4_PPA_RX_CHANNEL,
                                   DMA2D_MACRO_BLOCK_SIZE_NONE);
}

static void esp32p4_ppa_prepare_desc(
  dma2d_descriptor_t *desc,
  const struct esp32p4_ppa_picture_s *picture)
{
  uint32_t stride_pixels;

  memset(desc, 0, sizeof(*desc));
  stride_pixels = picture->stride_bytes /
                  (picture->color_mode == ESP32P4_PPA_COLOR_MODE_RGB565 ? 2 :
                   picture->color_mode == ESP32P4_PPA_COLOR_MODE_RGB888 ? 3 :
                   picture->color_mode == ESP32P4_PPA_COLOR_MODE_A8 ? 1 : 4);
  desc->vb_size = picture->block_height;
  desc->hb_length = picture->block_width;
  desc->dma2d_en = 1;
  desc->suc_eof = 1;
  desc->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
  desc->va_size = picture->pic_height;
  desc->ha_length = stride_pixels;
  desc->pbyte = esp32p4_ppa_pbyte(picture->color_mode);
  desc->y = picture->block_offset_y;
  desc->x = picture->block_offset_x;
  desc->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
  desc->buffer = picture->buffer;
}

static int esp32p4_ppa_sync_desc(struct esp32p4_ppa_dma_desc_s *storage)
{
  int ret;

  ret = esp_cache_msync(storage, sizeof(*storage),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  return ret == ESP_OK || ret == ESP_ERR_NOT_SUPPORTED ? 0 : -EIO;
}

static int esp32p4_ppa_sync_buffer(void *buffer, size_t size,
                                  uint32_t flags)
{
  int ret = esp_cache_msync(buffer, size, flags);

  return ret == ESP_OK || ret == ESP_ERR_NOT_SUPPORTED ? 0 : -EIO;
}

static int esp32p4_ppa_wait(dma2d_descriptor_t *desc, const char *operation)
{
  int ret;

  (void)operation;
  ret = nxsem_tickwait_uninterruptible(
    &g_ppa.done, MSEC2TICK(CONFIG_ESP32P4_PPA_TIMEOUT_MS));
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  syslog(LOG_INFO,
         "PPA_TRACE op=%s s=%lu w=%d irq=%lx eof=%lx d=%lx idle=%u/%u\n",
         operation, (unsigned long)g_ppa.active_sequence, ret,
         (unsigned long)g_ppa.isr_status,
         (unsigned long)g_ppa.isr_eof_desc,
         (unsigned long)(uintptr_t)desc,
         (unsigned int)g_ppa.isr_rx_idle,
         (unsigned int)g_ppa.isr_desc_idle);
#endif
  if (ret < 0)
    {
      return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

  if ((g_ppa.irq_status & DMA2D_LL_EVENT_RX_SUC_EOF) == 0 ||
      (g_ppa.irq_status & ESP32P4_PPA_RX_ERROR_EVENTS) != 0 ||
      dma2d_ll_rx_get_success_eof_desc_addr(g_ppa.dma_hal.dev,
        ESP32P4_PPA_RX_CHANNEL) != (uint32_t)(uintptr_t)desc ||
      !dma2d_ll_rx_is_fsm_idle(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL) ||
      !dma2d_ll_rx_is_desc_fsm_idle(g_ppa.dma_hal.dev,
                                    ESP32P4_PPA_RX_CHANNEL))
    {
      return -EIO;
    }

  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               UINT32_MAX, false);
  return 0;
}

static int esp32p4_ppa_rx_interrupt(int irq, void *context, void *arg)
{
  uint32_t status;

  status = dma2d_ll_rx_get_interrupt_status(g_ppa.dma_hal.dev,
                                             ESP32P4_PPA_RX_CHANNEL) &
           ESP32P4_PPA_RX_EVENTS;
  if (status == 0)
    {
      return OK;
    }

#ifdef CONFIG_ESP32P4_PPA_DEBUG
  g_ppa.isr_sequence = g_ppa.active_sequence;
  g_ppa.isr_status = status;
  g_ppa.isr_eof_desc =
    dma2d_ll_rx_get_success_eof_desc_addr(g_ppa.dma_hal.dev,
                                          ESP32P4_PPA_RX_CHANNEL);
  g_ppa.isr_rx_idle =
    dma2d_ll_rx_is_fsm_idle(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_RX_CHANNEL);
  g_ppa.isr_desc_idle =
    dma2d_ll_rx_is_desc_fsm_idle(g_ppa.dma_hal.dev,
                                 ESP32P4_PPA_RX_CHANNEL);
  g_ppa.isr_ppa_int_raw = g_ppa.ppa_hal.dev->int_raw.val;
  g_ppa.isr_blend_mode = g_ppa.ppa_hal.dev->blend_trans_mode.val;
#endif

  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               ESP32P4_PPA_RX_EVENTS, false);
  dma2d_ll_rx_clear_interrupt_status(g_ppa.dma_hal.dev,
                                     ESP32P4_PPA_RX_CHANNEL, status);
  g_ppa.irq_status |= status;
  nxsem_post(&g_ppa.done);
  return OK;
}

static int esp32p4_ppa_hw_initialize(void)
{
  bool enable_irq = false;
  int cpuint;

  PERIPH_RCC_ATOMIC()
    {
      ppa_ll_enable_bus_clock(true);
      ppa_ll_reset_register();
    }

  ppa_hal_init(&g_ppa.ppa_hal);

  PERIPH_RCC_ATOMIC()
    {
      dma2d_ll_enable_bus_clock(ESP32P4_PPA_DMA_GROUP, true);
      dma2d_ll_reset_register(ESP32P4_PPA_DMA_GROUP);
    }

  dma2d_hal_init(&g_ppa.dma_hal, ESP32P4_PPA_DMA_GROUP);
  dma2d_ll_hw_enable(g_ppa.dma_hal.dev, true);

  if (g_ppa.cpuint < 0)
    {
      cpuint = esp_setup_irq(
        dma2d_periph_signals.groups[ESP32P4_PPA_DMA_GROUP]
          .rx_irq_id[ESP32P4_PPA_RX_CHANNEL],
        ESP_IRQ_PRIORITY_DEFAULT, ESP_IRQ_TRIGGER_LEVEL,
        esp32p4_ppa_rx_interrupt, NULL);
      if (cpuint < 0)
        {
          dma2d_ll_hw_enable(g_ppa.dma_hal.dev, false);
          PERIPH_RCC_ATOMIC()
            {
              dma2d_ll_enable_bus_clock(ESP32P4_PPA_DMA_GROUP, false);
              ppa_ll_enable_bus_clock(false);
            }

          ppa_hal_deinit(&g_ppa.ppa_hal);
          return cpuint;
        }

      g_ppa.cpuint = cpuint;
      enable_irq = true;
    }

  g_ppa.irq_status = 0;
  esp32p4_ppa_rx_reset();
  if (enable_irq)
    {
      up_enable_irq(ESP_SOURCE2IRQ(
        dma2d_periph_signals.groups[ESP32P4_PPA_DMA_GROUP]
          .rx_irq_id[ESP32P4_PPA_RX_CHANNEL]));
    }

  g_ppa.initialized = true;
  return 0;
}

int esp32p4_ppa_hw_acquire(void)
{
  int ret;

  ret = nxmutex_lock(&g_ppa.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_ppa.initialized)
    {
      /* esp_setup_irq() and up_enable_irq() must observe the same CPU-local
       * interrupt handle map.
       */

      sched_lock();
      ret = esp32p4_ppa_hw_initialize();
      sched_unlock();
      if (ret < 0)
        {
          nxmutex_unlock(&g_ppa.lock);
          return ret;
        }
    }

  g_ppa.users++;
  nxmutex_unlock(&g_ppa.lock);
  return 0;
}

int esp32p4_ppa_hw_release(void)
{
  int ret;

  ret = nxmutex_lock(&g_ppa.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_ppa.users == 0)
    {
      nxmutex_unlock(&g_ppa.lock);
      return -EINVAL;
    }

  g_ppa.users--;

  /* Match the ESP-IDF engine lifetime: the last client leaves no live PPA or
   * DMA2D transaction behind.  Keep the CPU interrupt route allocated because
   * NuttX stores its handle per CPU and a client can unregister after task
   * migration.  With the peripheral interrupt disabled and both clocks gated,
   * the dormant route has no interrupt source to service.
   */

  if (g_ppa.users == 0)
    {
      esp32p4_ppa_rx_reset();
      g_ppa.irq_status = 0;
      dma2d_ll_hw_enable(g_ppa.dma_hal.dev, false);

      PERIPH_RCC_ATOMIC()
        {
          dma2d_ll_enable_bus_clock(ESP32P4_PPA_DMA_GROUP, false);
        }

      ppa_hal_deinit(&g_ppa.ppa_hal);

      PERIPH_RCC_ATOMIC()
        {
          ppa_ll_enable_bus_clock(false);
        }

      g_ppa.initialized = false;
    }

  nxmutex_unlock(&g_ppa.lock);
  return 0;
}

int esp32p4_ppa_dma2d_fill(esp32p4_ppa_handle_t handle,
                           const struct esp32p4_ppa_fill_config_s *config)
{
  const struct esp32p4_ppa_picture_s *output = &config->output;
  dma2d_descriptor_t *desc = &g_ppa_rx_desc.desc;
  uint32_t stride_pixels;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  uint32_t sequence;
  uint32_t configured_color;
#endif
  int ret;

  ret = nxmutex_lock(&g_ppa.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_ppa.initialized)
    {
      ret = -EIO;
      goto out_unlock;
    }

  esp32p4_ppa_rx_reset();
  g_ppa.irq_status = 0;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  sequence = ++g_ppa.sequence_counter;
  g_ppa.active_sequence = sequence;
  g_ppa.isr_sequence = 0;
  g_ppa.isr_status = 0;
  g_ppa.isr_eof_desc = 0;
  g_ppa.isr_ppa_int_raw = 0;
  g_ppa.isr_blend_mode = 0;
  g_ppa.isr_rx_idle = false;
  g_ppa.isr_desc_idle = false;
#endif
  memset(desc, 0, sizeof(*desc));
  stride_pixels = output->stride_bytes /
                  (output->color_mode == ESP32P4_PPA_COLOR_MODE_RGB565 ? 2 :
                   output->color_mode == ESP32P4_PPA_COLOR_MODE_RGB888 ? 3 : 4);

  desc->vb_size = output->block_height;
  desc->hb_length = output->block_width;
  desc->dma2d_en = 1;
  desc->suc_eof = 1;
  desc->owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
  desc->va_size = output->pic_height;
  desc->ha_length = stride_pixels;
  desc->pbyte = esp32p4_ppa_pbyte(output->color_mode);
  desc->y = output->block_offset_y;
  desc->x = output->block_offset_x;
  desc->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
  desc->buffer = output->buffer;

  ret = esp_cache_msync(output->buffer, output->buffer_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED)
    {
      ret = -EIO;
      goto out_unlock;
    }

  ret = esp_cache_msync(&g_ppa_rx_desc, sizeof(g_ppa_rx_desc),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED)
    {
      ret = -EIO;
      goto out_unlock;
    }

  dma2d_ll_rx_connect_to_periph(g_ppa.dma_hal.dev,
                                ESP32P4_PPA_RX_CHANNEL,
                                DMA2D_TRIG_PERIPH_PPA_BLEND,
                                SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_RX);
  dma2d_ll_rx_enable_reorder(g_ppa.dma_hal.dev,
                             ESP32P4_PPA_RX_CHANNEL, false);
  dma2d_ll_rx_enable_dscr_port(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL, false);
  dma2d_ll_rx_enable_owner_check(g_ppa.dma_hal.dev,
                                 ESP32P4_PPA_RX_CHANNEL, false);
  dma2d_ll_rx_set_auto_return_owner(g_ppa.dma_hal.dev,
                                    ESP32P4_PPA_RX_CHANNEL,
                                    DMA2D_DESCRIPTOR_BUFFER_OWNER_CPU);
  dma2d_ll_rx_enable_descriptor_burst(g_ppa.dma_hal.dev,
                                      ESP32P4_PPA_RX_CHANNEL, true);
  dma2d_ll_rx_set_data_burst_length(g_ppa.dma_hal.dev,
                                    ESP32P4_PPA_RX_CHANNEL,
                                    esp32p4_ppa_dma_burst(
                                      handle->burst_length));
  dma2d_ll_rx_enable_page_bound_wrap(g_ppa.dma_hal.dev,
                                     ESP32P4_PPA_RX_CHANNEL, true);
  dma2d_ll_rx_set_macro_block_size(g_ppa.dma_hal.dev,
                                   ESP32P4_PPA_RX_CHANNEL,
                                   DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_rx_clear_interrupt_status(g_ppa.dma_hal.dev,
                                     ESP32P4_PPA_RX_CHANNEL, UINT32_MAX);
  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               ESP32P4_PPA_RX_EVENTS, true);
  dma2d_ll_rx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_RX_CHANNEL,
                            (uint32_t)(uintptr_t)desc);

  esp32p4_ppa_hal_configure_fill(g_ppa.ppa_hal.dev, config,
#ifdef CONFIG_ESP32P4_PPA_DEBUG
                                  &configured_color
#else
                                  NULL
#endif
                                  );

  dma2d_ll_rx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL);
  esp32p4_ppa_hal_start_fill(g_ppa.ppa_hal.dev);

  ret = nxsem_tickwait_uninterruptible(
    &g_ppa.done, MSEC2TICK(CONFIG_ESP32P4_PPA_TIMEOUT_MS));
  if (ret < 0)
    {
      ret = ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
      syslog(LOG_INFO,
             "PPA_TRACE s=%lu w=%d c=%04lx hw=%08lx is=%lu irq=%lx "
             "eof=%lx d=%lx idle=%u/%u live=%lx/%u/%u\n",
             (unsigned long)sequence, ret,
             (unsigned long)config->color,
             (unsigned long)configured_color,
             (unsigned long)g_ppa.isr_sequence,
             (unsigned long)g_ppa.isr_status,
             (unsigned long)g_ppa.isr_eof_desc,
             (unsigned long)(uintptr_t)desc,
             (unsigned int)g_ppa.isr_rx_idle,
             (unsigned int)g_ppa.isr_desc_idle,
             (unsigned long)dma2d_ll_rx_get_interrupt_status(
               g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL),
             (unsigned int)dma2d_ll_rx_is_fsm_idle(
               g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL),
             (unsigned int)dma2d_ll_rx_is_desc_fsm_idle(
               g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL));
#endif
      esp32p4_ppa_rx_reset();
      goto out_unlock;
    }

#ifdef CONFIG_ESP32P4_PPA_DEBUG
  syslog(LOG_INFO,
         "PPA_TRACE s=%lu w=0 c=%04lx hw=%08lx is=%lu irq=%lx "
         "eof=%lx d=%lx idle=%u/%u\n",
         (unsigned long)sequence,
         (unsigned long)config->color,
         (unsigned long)configured_color,
         (unsigned long)g_ppa.isr_sequence,
         (unsigned long)g_ppa.isr_status,
         (unsigned long)g_ppa.isr_eof_desc,
         (unsigned long)(uintptr_t)desc,
         (unsigned int)g_ppa.isr_rx_idle,
         (unsigned int)g_ppa.isr_desc_idle);
#endif

  if ((g_ppa.irq_status & DMA2D_LL_EVENT_RX_SUC_EOF) == 0 ||
      (g_ppa.irq_status & ESP32P4_PPA_RX_ERROR_EVENTS) != 0)
    {
      ret = -EIO;
      esp32p4_ppa_rx_reset();
      goto out_unlock;
    }

  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               UINT32_MAX, false);
  ret = esp_cache_msync(output->buffer, output->buffer_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED)
    {
      ret = -EIO;
      goto out_unlock;
    }

  ret = 0;

out_unlock:
  nxmutex_unlock(&g_ppa.lock);
  return ret;
}

static ppa_srm_color_mode_t
esp32p4_ppa_srm_mode(enum esp32p4_ppa_color_mode_e mode)
{
  switch (mode)
    {
      case ESP32P4_PPA_COLOR_MODE_RGB565:
        return PPA_SRM_COLOR_MODE_RGB565;
      case ESP32P4_PPA_COLOR_MODE_RGB888:
        return PPA_SRM_COLOR_MODE_RGB888;
      default:
        return PPA_SRM_COLOR_MODE_ARGB8888;
    }
}

int esp32p4_ppa_dma2d_blend(esp32p4_ppa_handle_t handle,
                            const struct esp32p4_ppa_blend_config_s *config)
{
  dma2d_descriptor_t *bg_desc = &g_ppa_tx_bg_desc.desc;
  dma2d_descriptor_t *fg_desc = &g_ppa_tx_fg_desc.desc;
  dma2d_descriptor_t *out_desc = &g_ppa_rx_desc.desc;
  dma2d_data_burst_length_t burst;
  int ret;

  ret = nxmutex_lock(&g_ppa.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_ppa.initialized)
    {
      ret = -EIO;
      goto out_unlock;
    }

  esp32p4_ppa_rx_reset();
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_BG_CHANNEL);
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_FG_CHANNEL);
  g_ppa.irq_status = 0;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  g_ppa.active_sequence = ++g_ppa.sequence_counter;
  g_ppa.isr_sequence = 0;
  g_ppa.isr_status = 0;
  g_ppa.isr_eof_desc = 0;
  g_ppa.isr_rx_idle = false;
  g_ppa.isr_desc_idle = false;
#endif

  ret = esp32p4_ppa_sync_buffer(config->background.buffer,
                                config->background.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret < 0)
    {
      goto out_reset;
    }

  ret = esp32p4_ppa_sync_buffer(config->foreground.buffer,
                                config->foreground.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret < 0)
    {
      goto out_reset;
    }

  ret = esp32p4_ppa_sync_buffer(config->output.buffer,
                                config->output.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (ret < 0)
    {
      goto out_reset;
    }

  esp32p4_ppa_prepare_desc(bg_desc, &config->background);
  esp32p4_ppa_prepare_desc(fg_desc, &config->foreground);
  esp32p4_ppa_prepare_desc(out_desc, &config->output);
  ret = esp32p4_ppa_sync_desc(&g_ppa_tx_bg_desc);
  ret = ret < 0 ? ret : esp32p4_ppa_sync_desc(&g_ppa_tx_fg_desc);
  ret = ret < 0 ? ret : esp32p4_ppa_sync_desc(&g_ppa_rx_desc);
  if (ret < 0)
    {
      goto out_reset;
    }

  burst = esp32p4_ppa_dma_burst(handle->burst_length);
  esp32p4_ppa_configure_tx(ESP32P4_PPA_TX_BG_CHANNEL,
                           DMA2D_TRIG_PERIPH_PPA_BLEND,
                           SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_BG_TX,
                           false, burst);
  esp32p4_ppa_configure_tx(ESP32P4_PPA_TX_FG_CHANNEL,
                           DMA2D_TRIG_PERIPH_PPA_BLEND,
                           SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_FG_TX,
                           false, burst);
  esp32p4_ppa_configure_rx(DMA2D_TRIG_PERIPH_PPA_BLEND,
                           SOC_DMA2D_TRIG_PERIPH_PPA_BLEND_RX,
                           false, burst);
  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               ESP32P4_PPA_RX_EVENTS, true);
  dma2d_ll_tx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_TX_BG_CHANNEL,
                            (uint32_t)(uintptr_t)bg_desc);
  dma2d_ll_tx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_TX_FG_CHANNEL,
                            (uint32_t)(uintptr_t)fg_desc);
  dma2d_ll_rx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_RX_CHANNEL,
                            (uint32_t)(uintptr_t)out_desc);

  esp32p4_ppa_hal_configure_blend(g_ppa.ppa_hal.dev, config);

  dma2d_ll_tx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_TX_BG_CHANNEL);
  dma2d_ll_tx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_TX_FG_CHANNEL);
  dma2d_ll_rx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL);
  esp32p4_ppa_hal_start_blend(g_ppa.ppa_hal.dev);

  ret = esp32p4_ppa_wait(out_desc, "blend");
  if (ret < 0)
    {
      goto out_reset;
    }

  ret = esp32p4_ppa_sync_buffer(config->output.buffer,
                                config->output.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  if (ret < 0)
    {
      goto out_reset;
    }

  goto out_unlock;

out_reset:
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_BG_CHANNEL);
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_FG_CHANNEL);
  esp32p4_ppa_rx_reset();
out_unlock:
  nxmutex_unlock(&g_ppa.lock);
  return ret;
}

int esp32p4_ppa_dma2d_srm(esp32p4_ppa_handle_t handle,
                          const struct esp32p4_ppa_srm_config_s *config)
{
  dma2d_descriptor_t *in_desc = &g_ppa_tx_bg_desc.desc;
  dma2d_descriptor_t *out_desc = &g_ppa_rx_desc.desc;
  dma2d_data_burst_length_t burst;
  ppa_srm_color_mode_t input_mode;
  uint32_t block_h;
  uint32_t block_v;
  int ret;

  ret = nxmutex_lock(&g_ppa.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_ppa.initialized)
    {
      ret = -EIO;
      goto out_unlock;
    }

  esp32p4_ppa_rx_reset();
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_BG_CHANNEL);
  ppa_ll_srm_reset(g_ppa.ppa_hal.dev);
  g_ppa.irq_status = 0;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
  g_ppa.active_sequence = ++g_ppa.sequence_counter;
  g_ppa.isr_sequence = 0;
  g_ppa.isr_status = 0;
  g_ppa.isr_eof_desc = 0;
  g_ppa.isr_rx_idle = false;
  g_ppa.isr_desc_idle = false;
#endif

  ret = esp32p4_ppa_sync_buffer(config->input.buffer,
                                config->input.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret < 0)
    {
      goto out_reset;
    }

  ret = esp32p4_ppa_sync_buffer(config->output.buffer,
                                config->output.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (ret < 0)
    {
      goto out_reset;
    }

  esp32p4_ppa_prepare_desc(in_desc, &config->input);
  esp32p4_ppa_prepare_desc(out_desc, &config->output);
  out_desc->vb_size = 2;
  out_desc->hb_length = 2;
  ret = esp32p4_ppa_sync_desc(&g_ppa_tx_bg_desc);
  ret = ret < 0 ? ret : esp32p4_ppa_sync_desc(&g_ppa_rx_desc);
  if (ret < 0)
    {
      goto out_reset;
    }

  burst = esp32p4_ppa_dma_burst(handle->burst_length);
  esp32p4_ppa_configure_tx(ESP32P4_PPA_TX_BG_CHANNEL,
                           DMA2D_TRIG_PERIPH_PPA_SRM,
                           SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX,
                           true, burst);
  esp32p4_ppa_configure_rx(DMA2D_TRIG_PERIPH_PPA_SRM,
                           SOC_DMA2D_TRIG_PERIPH_PPA_SRM_RX,
                           true, burst);
  input_mode = esp32p4_ppa_srm_mode(config->input.color_mode);
  ppa_ll_srm_get_dma_dscr_port_mode_block_size(g_ppa.ppa_hal.dev,
    input_mode, ppa_ll_srm_get_mb_size(g_ppa.ppa_hal.dev),
    &block_h, &block_v);
  dma2d_ll_tx_set_dscr_port_block_size(g_ppa.dma_hal.dev,
                                       ESP32P4_PPA_TX_BG_CHANNEL,
                                       block_h, block_v);
  dma2d_ll_rx_enable_interrupt(g_ppa.dma_hal.dev,
                               ESP32P4_PPA_RX_CHANNEL,
                               ESP32P4_PPA_RX_EVENTS, true);
  dma2d_ll_tx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_TX_BG_CHANNEL,
                            (uint32_t)(uintptr_t)in_desc);
  dma2d_ll_rx_set_desc_addr(g_ppa.dma_hal.dev,
                            ESP32P4_PPA_RX_CHANNEL,
                            (uint32_t)(uintptr_t)out_desc);

  esp32p4_ppa_hal_configure_srm(g_ppa.ppa_hal.dev, config);

  dma2d_ll_tx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_TX_BG_CHANNEL);
  dma2d_ll_rx_start(g_ppa.dma_hal.dev, ESP32P4_PPA_RX_CHANNEL);
  esp32p4_ppa_hal_start_srm(g_ppa.ppa_hal.dev);

  ret = esp32p4_ppa_wait(out_desc, "srm");
  if (ret < 0)
    {
      goto out_reset;
    }

  ret = esp32p4_ppa_sync_buffer(config->output.buffer,
                                config->output.buffer_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  if (ret < 0)
    {
      goto out_reset;
    }

  goto out_unlock;

out_reset:
  esp32p4_ppa_tx_reset(ESP32P4_PPA_TX_BG_CHANNEL);
  esp32p4_ppa_rx_reset();
  ppa_ll_srm_reset(g_ppa.ppa_hal.dev);
out_unlock:
  nxmutex_unlock(&g_ppa.lock);
  return ret;
}
