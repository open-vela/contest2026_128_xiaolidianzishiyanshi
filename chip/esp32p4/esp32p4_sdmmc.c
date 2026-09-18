/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_sdmmc.c
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
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include <arch/chip/irq.h>
#include <arch/chip/gpio_sig_map.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_irq.h"
#include "esp_cache.h"
#include "esp_private/periph_ctrl.h"
#include "hal/gpio_ll.h"
#include "hal/sdmmc_ll.h"
#include "hal/sdmmc_periph.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_pins.h"
#include "soc/sdmmc_pins.h"

#ifdef CONFIG_ESP32P4_SDMMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32P4_SDMMC_NSLOTS          2
#define ESP32P4_SDMMC_SRC_FREQ        160000000u
#define ESP32P4_SDMMC_CACHE_ALIGN     64u
#define ESP32P4_SDMMC_MAX_BLOCKS      128u
#define ESP32P4_SDMMC_MAX_XFR         (ESP32P4_SDMMC_MAX_BLOCKS * 512u)
#define ESP32P4_SDMMC_NDESCS          \
  ((ESP32P4_SDMMC_MAX_XFR + SDMMC_DMA_MAX_BUF_LEN - 1) / \
   SDMMC_DMA_MAX_BUF_LEN)

#define SDCARD_CMDTIMEOUT             MSEC2TICK(100)
#define SDCARD_LONGTIMEOUT            MSEC2TICK(2000)
#define SDCARD_RESETTIMEOUT           MSEC2TICK(5000)
#define SDCARD_CLKUPDATE_RETRIES      3

#define SDCARD_RESPERR_MASK           \
  (SDMMC_LL_EVENT_RESP_ERR | SDMMC_LL_EVENT_RCRC | SDMMC_LL_EVENT_RTO)
#define SDCARD_XFRERR_MASK            \
  (SDMMC_LL_EVENT_DCRC | SDMMC_LL_EVENT_DTO | SDMMC_LL_EVENT_HTO | \
   SDMMC_LL_EVENT_FRUN | SDMMC_LL_EVENT_SBE | SDMMC_LL_EVENT_EBE)
#define SDCARD_XFR_MASK               \
  (SDMMC_LL_EVENT_DATA_OVER | SDCARD_XFRERR_MASK)
#define SDCARD_CMD_MASK               \
  (SDMMC_LL_EVENT_CMD_DONE | SDCARD_RESPERR_MASK | SDMMC_LL_EVENT_HLE)
#define SDCARD_DMA_ERROR_MASK         ((1 << 2) | (1 << 4))
#define SDCARD_DMA_DONE_MASK          \
  (SDMMC_LL_EVENT_DMA_TI | SDMMC_LL_EVENT_DMA_RI | \
   SDMMC_LL_EVENT_DMA_NI)
#define SDCARD_DMA_IRQ_MASK           0x0000033f

struct esp32p4_dev_s
{
  struct sdio_dev_s dev;
  int slot;
  bool inited;

  sem_t cmdsem;
  sem_t waitsem;
  sem_t iosem;
  sdio_eventset_t waitevents;
  volatile sdio_eventset_t wkupevent;
  uint32_t waitmask;
  bool iowait;
  bool ioirqwait;
  volatile uint32_t cmdstatus;
  struct wdog_s waitwdog;

  sdio_statset_t cdstatus;
  sdio_eventset_t cbevents;
  worker_t callback;
  void *cbarg;
  struct work_s cbwork;

  uint8_t *buffer;
  uint8_t *dmabuffer;
  size_t remaining;
  size_t blocklen;
  size_t nblocks;
  bool wrdir;
  bool wide;
  bool xfrdone;
  bool dmadone;
  enum sdio_clock_e rate;
  sdmmc_desc_t dma_desc[ESP32P4_SDMMC_NDESCS]
    __attribute__((aligned(ESP32P4_SDMMC_CACHE_ALIGN)));
};

struct esp32p4_host_s
{
  sdmmc_dev_t *hw;
  mutex_t lock;
  bool clocked;
  bool attached;
  bool resetok;
  volatile struct esp32p4_dev_s *active;
};

#ifdef CONFIG_SDIO_MUXBUS
static int esp32p4_lock(struct sdio_dev_s *dev, bool lock);
#endif
static void esp32p4_reset(struct sdio_dev_s *dev);
static sdio_capset_t esp32p4_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t esp32p4_status(struct sdio_dev_s *dev);
static void esp32p4_widebus(struct sdio_dev_s *dev, bool wide);
static void esp32p4_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int esp32p4_attach(struct sdio_dev_s *dev);
static int esp32p4_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t arg);
#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp32p4_blocksetup(struct sdio_dev_s *dev,
                               unsigned int blocklen,
                               unsigned int nblocks);
#endif
static int esp32p4_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                             size_t nbytes);
static int esp32p4_sendsetup(struct sdio_dev_s *dev,
                             const uint8_t *buffer, size_t nbytes);
static int esp32p4_cancel(struct sdio_dev_s *dev);
static int esp32p4_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int esp32p4_recvshortcrc(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t *rshort);
static int esp32p4_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t rlong[4]);
static int esp32p4_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                             uint32_t *rshort);
static void esp32p4_waitenable(struct sdio_dev_s *dev,
                               sdio_eventset_t eventset,
                               uint32_t timeout);
static sdio_eventset_t esp32p4_eventwait(struct sdio_dev_s *dev);
static void esp32p4_callbackenable(struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset);
static int esp32p4_registercallback(struct sdio_dev_s *dev,
                                    worker_t callback, void *arg);

#ifdef CONFIG_SDIO_MUXBUS
#  define ESP32P4_SDMMC_LOCK_INITIALIZER .lock = esp32p4_lock,
#else
#  define ESP32P4_SDMMC_LOCK_INITIALIZER
#endif

#ifdef CONFIG_SDIO_BLOCKSETUP
#  define ESP32P4_SDMMC_BLOCK_INITIALIZER .blocksetup = esp32p4_blocksetup,
#else
#  define ESP32P4_SDMMC_BLOCK_INITIALIZER
#endif

#ifdef CONFIG_SDIO_DMA
#  define ESP32P4_SDMMC_DMA_INITIALIZER \
    .dmarecvsetup = esp32p4_recvsetup, \
    .dmasendsetup = esp32p4_sendsetup,
#else
#  define ESP32P4_SDMMC_DMA_INITIALIZER
#endif

#define ESP32P4_SDMMC_DEV_INITIALIZER(n) \
  { \
    .dev = \
      { \
        ESP32P4_SDMMC_LOCK_INITIALIZER \
        .reset            = esp32p4_reset, \
        .capabilities     = esp32p4_capabilities, \
        .status           = esp32p4_status, \
        .widebus          = esp32p4_widebus, \
        .clock            = esp32p4_clock, \
        .attach           = esp32p4_attach, \
        .sendcmd          = esp32p4_sendcmd, \
        ESP32P4_SDMMC_BLOCK_INITIALIZER \
        .recvsetup        = esp32p4_recvsetup, \
        .sendsetup        = esp32p4_sendsetup, \
        .cancel           = esp32p4_cancel, \
        .waitresponse     = esp32p4_waitresponse, \
        .recv_r1          = esp32p4_recvshortcrc, \
        .recv_r2          = esp32p4_recvlong, \
        .recv_r3          = esp32p4_recvshort, \
        .recv_r4          = esp32p4_recvshort, \
        .recv_r5          = esp32p4_recvshortcrc, \
        .recv_r6          = esp32p4_recvshortcrc, \
        .recv_r7          = esp32p4_recvshort, \
        .waitenable       = esp32p4_waitenable, \
        .eventwait        = esp32p4_eventwait, \
        .callbackenable   = esp32p4_callbackenable, \
        .registercallback = esp32p4_registercallback, \
        ESP32P4_SDMMC_DMA_INITIALIZER \
      }, \
    .slot = (n), \
    .cmdsem = SEM_INITIALIZER(0), \
    .waitsem = SEM_INITIALIZER(0), \
    .iosem = SEM_INITIALIZER(0), \
  }

static struct esp32p4_dev_s g_sdiodev[ESP32P4_SDMMC_NSLOTS] =
{
  ESP32P4_SDMMC_DEV_INITIALIZER(0),
  ESP32P4_SDMMC_DEV_INITIALIZER(1),
};

static struct esp32p4_host_s g_sdmmchost =
{
  .hw = &SDMMC,
  .lock = NXMUTEX_INITIALIZER,
};

static void esp32p4_enable_ints(struct esp32p4_dev_s *priv)
{
  uint32_t mask = SDCARD_CMD_MASK | priv->waitmask;

  if (priv->ioirqwait)
    {
      mask |= SDMMC_LL_EVENT_IO_SLOT0 << priv->slot;
    }

  if (priv->remaining != 0)
    {
      mask |= SDCARD_XFR_MASK;
    }

#ifdef CONFIG_MMCSD_HAVE_CARDDETECT
  mask |= SDMMC_LL_EVENT_CD;
#endif
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, SDMMC_LL_SD_EVENT_MASK, false);
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, mask, true);
}

static void esp32p4_release_buffer(struct esp32p4_dev_s *priv)
{
  if (priv->dmabuffer != NULL)
    {
      kmm_free(priv->dmabuffer);
      priv->dmabuffer = NULL;
    }
}

static int esp32p4_cache_sync(struct esp32p4_dev_s *priv, void *addr,
                              size_t size, uint32_t flags,
                              const char *what)
{
  esp_err_t ret;

  ret = esp_cache_msync(addr, size, flags);
  if (ret != ESP_OK)
    {
      mcerr("ERROR: slot %d %s cache sync failed: %d\n",
            priv->slot, what, ret);
      return -EIO;
    }

  return OK;
}

static void esp32p4_endwait(struct esp32p4_dev_s *priv,
                            sdio_eventset_t event)
{
  wd_cancel(&priv->waitwdog);
  priv->waitevents = 0;
  priv->waitmask = 0;
  priv->wkupevent = event;
  esp32p4_enable_ints(priv);
  nxsem_post(&priv->waitsem);
}

static void esp32p4_endtransfer(struct esp32p4_dev_s *priv,
                                sdio_eventset_t event)
{
  sdmmc_ll_stop_dma(g_sdmmchost.hw);

  if ((priv->waitevents & event) != 0 ||
      (event & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) != 0)
    {
      esp32p4_endwait(priv, event);
    }
}

static void esp32p4_eventtimeout(wdparm_t arg)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)(uintptr_t)arg;

  if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
    {
      esp32p4_endwait(priv, SDIOWAIT_TIMEOUT);
    }
}

static void esp32p4_callback(struct esp32p4_dev_s *priv)
{
  sdio_eventset_t event;

  if (priv->callback == NULL)
    {
      return;
    }

  event = (priv->cdstatus & SDIO_STATUS_PRESENT) != 0 ?
          SDIOMEDIA_INSERTED : SDIOMEDIA_EJECTED;
  if ((priv->cbevents & event) == 0)
    {
      return;
    }

  priv->cbevents = 0;
  if (up_interrupt_context())
    {
      work_queue(HPWORK, &priv->cbwork, priv->callback, priv->cbarg, 0);
    }
  else
    {
      priv->callback(priv->cbarg);
    }
}

static int esp32p4_interrupt(int irq, void *context, void *arg)
{
  struct esp32p4_dev_s *priv;
  uint32_t pending;
  uint32_t dma_pending;
  uint32_t cmd_pending;
  uint32_t xfr_pending;
  int i;

  pending = sdmmc_ll_get_intr_status(g_sdmmchost.hw);
  dma_pending = sdmmc_ll_get_idsts_interrupt_raw(g_sdmmchost.hw) &
                SDCARD_DMA_IRQ_MASK;

  sdmmc_ll_clear_interrupt(g_sdmmchost.hw, pending);
  sdmmc_ll_clear_idsts_interrupt(g_sdmmchost.hw, dma_pending);

#ifdef CONFIG_MMCSD_HAVE_CARDDETECT
  if ((pending & SDMMC_LL_EVENT_CD) != 0)
    {
      for (i = 0; i < ESP32P4_SDMMC_NSLOTS; i++)
        {
          sdio_statset_t oldstatus;

          priv = &g_sdiodev[i];
          if (!priv->inited)
            {
              continue;
            }

          oldstatus = priv->cdstatus;
          esp32p4_status(&priv->dev);
          if (oldstatus != priv->cdstatus)
            {
              esp32p4_callback(priv);
            }
        }
    }
#else
  (void)i;
#endif

  for (i = 0; i < ESP32P4_SDMMC_NSLOTS; i++)
    {
      priv = &g_sdiodev[i];
      if (priv->ioirqwait &&
          (pending & (SDMMC_LL_EVENT_IO_SLOT0 << i)) != 0)
        {
          priv->ioirqwait = false;
          sdmmc_ll_enable_interrupt(g_sdmmchost.hw,
                                    SDMMC_LL_EVENT_IO_SLOT0 << i, false);
          nxsem_post(&priv->iosem);
        }

      if (priv->iowait &&
          (pending & (SDMMC_LL_EVENT_IO_SLOT0 << i)) != 0)
        {
          priv->iowait = false;
          esp32p4_endwait(priv, SDIOWAIT_CMDDONE);
        }
    }

  priv = (struct esp32p4_dev_s *)g_sdmmchost.active;
  if (priv == NULL)
    {
      return OK;
    }

  cmd_pending = pending & SDCARD_CMD_MASK;
  if (cmd_pending != 0)
    {
      priv->cmdstatus |= cmd_pending;
      if ((cmd_pending & (SDMMC_LL_EVENT_CMD_DONE |
                          SDMMC_LL_EVENT_HLE)) != 0)
        {
          nxsem_post(&priv->cmdsem);
        }

      if ((priv->waitevents & (SDIOWAIT_CMDDONE |
                               SDIOWAIT_RESPONSEDONE)) != 0 &&
          !priv->iowait &&
          (cmd_pending & (SDMMC_LL_EVENT_CMD_DONE |
                          SDMMC_LL_EVENT_HLE)) != 0)
        {
          sdio_eventset_t event;

          event = (priv->waitevents & SDIOWAIT_RESPONSEDONE) != 0 ?
                  SDIOWAIT_RESPONSEDONE : SDIOWAIT_CMDDONE;
          if ((cmd_pending & (SDCARD_RESPERR_MASK |
                              SDMMC_LL_EVENT_HLE)) != 0)
            {
              event |= SDIOWAIT_ERROR;
            }

          esp32p4_endwait(priv, event);
        }
    }

  xfr_pending = pending & SDCARD_XFR_MASK;
  if (priv->remaining != 0 &&
      ((xfr_pending & SDCARD_XFRERR_MASK) != 0 ||
       (dma_pending & SDCARD_DMA_ERROR_MASK) != 0))
    {
      sdio_eventset_t event = SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR;

      if ((xfr_pending & (SDMMC_LL_EVENT_DTO |
                          SDMMC_LL_EVENT_HTO)) != 0)
        {
          event |= SDIOWAIT_TIMEOUT;
        }

      esp32p4_endtransfer(priv, event);
    }
  else if (priv->remaining != 0)
    {
      if ((xfr_pending & SDMMC_LL_EVENT_DATA_OVER) != 0)
        {
          priv->xfrdone = true;
        }

      if ((dma_pending & SDCARD_DMA_DONE_MASK) != 0)
        {
          priv->dmadone = true;
        }

      if (priv->xfrdone && priv->dmadone)
        {
          esp32p4_endtransfer(priv, SDIOWAIT_TRANSFERDONE);
        }
    }

  return OK;
}

static int esp32p4_ciu_sendcmd(struct esp32p4_dev_s *priv,
                               sdmmc_hw_cmd_t command, uint32_t arg)
{
  clock_t start = clock_systime_ticks();
  uint32_t status;

  while (!sdmmc_ll_is_command_taken(g_sdmmchost.hw))
    {
      if (clock_systime_ticks() - start > SDCARD_CMDTIMEOUT)
        {
          status = sdmmc_ll_get_interrupt_raw(g_sdmmchost.hw);
          mcerr("ERROR: slot %d CIU busy before cmd=%u update=%u "
                "raw=%08lx\n",
                priv->slot, command.cmd_index, command.update_clk_reg,
                (unsigned long)status);
          return -ETIMEDOUT;
        }
    }

  sdmmc_ll_set_command_arg(g_sdmmchost.hw, arg);
  command.card_num = priv->slot;
  command.use_hold_reg = 1;
  command.start_command = 1;
  sdmmc_ll_set_command(g_sdmmchost.hw, command);

  start = clock_systime_ticks();
  while (!sdmmc_ll_is_command_taken(g_sdmmchost.hw))
    {
      if (clock_systime_ticks() - start > SDCARD_CMDTIMEOUT)
        {
          status = sdmmc_ll_get_interrupt_raw(g_sdmmchost.hw);
          mcerr("ERROR: slot %d CIU did not accept cmd=%u update=%u "
                "raw=%08lx\n",
                priv->slot, command.cmd_index, command.update_clk_reg,
                (unsigned long)status);
          return -ETIMEDOUT;
        }
    }

  return OK;
}

static int esp32p4_clock_update(struct esp32p4_dev_s *priv)
{
  sdmmc_hw_cmd_t command =
  {
    0
  };
  uint32_t status;
  int retries;
  int ret;

  command.update_clk_reg = 1;
  command.wait_complete = 1;
  for (retries = 0; retries < SDCARD_CLKUPDATE_RETRIES; retries++)
    {
      sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDMMC_LL_EVENT_HLE);
      ret = esp32p4_ciu_sendcmd(priv, command, 0);
      if (ret < 0)
        {
          return ret;
        }

      status = sdmmc_ll_get_interrupt_raw(g_sdmmchost.hw);
      if ((status & SDMMC_LL_EVENT_HLE) != 0)
        {
          mcwarn("WARNING: slot %d clock update HLE, retry %d/%d\n",
                 priv->slot, retries + 1, SDCARD_CLKUPDATE_RETRIES);
          sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDMMC_LL_EVENT_HLE);
        }
      else
        {
          return OK;
        }
    }

  return -EIO;
}

#ifdef CONFIG_SDIO_MUXBUS
static int esp32p4_lock(struct sdio_dev_s *dev, bool lock)
{
  return lock ? nxmutex_lock(&g_sdmmchost.lock) :
                nxmutex_unlock(&g_sdmmchost.lock);
}
#endif

static void esp32p4_reset(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  clock_t start;
  int i;

  g_sdmmchost.resetok = false;

  sdmmc_ll_enable_global_interrupt(g_sdmmchost.hw, false);
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, SDMMC_LL_SD_EVENT_MASK, false);
  sdmmc_ll_reset_controller(g_sdmmchost.hw);
  sdmmc_ll_reset_dma(g_sdmmchost.hw);
  sdmmc_ll_reset_fifo(g_sdmmchost.hw);

  start = clock_systime_ticks();
  while (!sdmmc_ll_is_controller_reset_done(g_sdmmchost.hw) ||
         !sdmmc_ll_is_dma_reset_done(g_sdmmchost.hw) ||
         !sdmmc_ll_is_fifo_reset_done(g_sdmmchost.hw))
    {
      if (clock_systime_ticks() - start > SDCARD_RESETTIMEOUT)
        {
          mcerr("ERROR: SDMMC reset timeout: controller=%d dma=%d "
                "fifo=%d raw=%08lx\n",
                sdmmc_ll_is_controller_reset_done(g_sdmmchost.hw),
                sdmmc_ll_is_dma_reset_done(g_sdmmchost.hw),
                sdmmc_ll_is_fifo_reset_done(g_sdmmchost.hw),
                (unsigned long)sdmmc_ll_get_interrupt_raw(g_sdmmchost.hw));
          return;
        }
    }

  g_sdmmchost.resetok = true;

  sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDMMC_LL_SD_EVENT_MASK);
  sdmmc_ll_clear_idsts_interrupt(g_sdmmchost.hw, UINT32_MAX);
  sdmmc_ll_set_card_width(g_sdmmchost.hw, priv->slot,
                          SD_BUS_WIDTH_1_BIT);
  sdmmc_ll_set_data_timeout(g_sdmmchost.hw, 0xffffff);
  sdmmc_ll_set_response_timeout(g_sdmmchost.hw, 0xff);
  sdmmc_ll_init_dma(g_sdmmchost.hw);
  sdmmc_ll_enable_dma(g_sdmmchost.hw, false);

  priv->waitevents = 0;
  priv->wkupevent = 0;
  priv->waitmask = 0;
  priv->iowait = false;
  priv->ioirqwait = false;
  priv->cmdstatus = 0;
  priv->remaining = 0;
  priv->xfrdone = false;
  priv->dmadone = false;
  priv->blocklen = 0;
  priv->nblocks = 0;
  priv->wide = false;
  priv->rate = CLOCK_SDIO_DISABLED;
  wd_cancel(&priv->waitwdog);
  nxsem_reset(&priv->cmdsem, 0);
  nxsem_reset(&priv->waitsem, 0);
  nxsem_reset(&priv->iosem, 0);
  esp32p4_release_buffer(priv);

  esp32p4_enable_ints(priv);
  sdmmc_ll_enable_global_interrupt(g_sdmmchost.hw, true);

  /* A controller reset affects both card slots.  Restore the other slot's
   * host-side bus settings; the card itself was not reset.
   */

  for (i = 0; i < ESP32P4_SDMMC_NSLOTS; i++)
    {
      struct esp32p4_dev_s *other = &g_sdiodev[i];

      if (other != priv && other->inited)
        {
          sdmmc_ll_set_card_width(g_sdmmchost.hw, other->slot,
                                  other->wide ? SD_BUS_WIDTH_4_BIT :
                                                SD_BUS_WIDTH_1_BIT);
          esp32p4_clock(&other->dev, other->rate);
        }
    }
}

static sdio_capset_t esp32p4_capabilities(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  sdio_capset_t caps = SDIO_CAPS_DMABEFOREWRITE |
                       SDIO_CAPS_DMASUPPORTED |
                       SDIO_CAPS_MMC_HS_MODE;
  uint32_t hcon = sdmmc_ll_get_hw_config_info(g_sdmmchost.hw);

  if (((hcon >> 1) & 0x1f) < (uint32_t)priv->slot)
    {
      return 0;
    }

#ifdef CONFIG_SDIO_WIDTH_D1_ONLY
  caps |= SDIO_CAPS_1BIT_ONLY;
#else
  if (sdmmc_slot_info[priv->slot].width >= 4)
    {
      caps |= SDIO_CAPS_4BIT;
    }

#endif
  return caps;
}

static sdio_statset_t esp32p4_status(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  sdio_statset_t status = 0;

  if (sdmmc_ll_is_card_detected(g_sdmmchost.hw, priv->slot))
    {
      status |= SDIO_STATUS_PRESENT;
#ifdef CONFIG_MMCSD_HAVE_WRITEPROTECT
      if (sdmmc_ll_is_card_write_protected(g_sdmmchost.hw, priv->slot))
        {
          status |= SDIO_STATUS_WRPROTECTED;
        }
#endif
    }

  priv->cdstatus = status;
  return status;
}

static void esp32p4_widebus(struct sdio_dev_s *dev, bool wide)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

#ifdef CONFIG_SDIO_WIDTH_D1_ONLY
  wide = false;
#endif

  /* A card samples DAT3 while CMD0 is sent to choose between native SD and
   * SPI mode.  Keep slot 0 DAT3 high as a GPIO throughout identification;
   * only connect it to the SDMMC peripheral after ACMD6 selects 4-bit mode.
   */

  if (priv->slot == 0 && !wide)
    {
      esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D3,
                     OUTPUT | PULLUP | DRIVE_3);
      esp_gpiowrite(SDMMC_SLOT0_IOMUX_PIN_NUM_D3, true);
    }

  sdmmc_ll_set_card_width(g_sdmmchost.hw, priv->slot,
                          wide ? SD_BUS_WIDTH_4_BIT : SD_BUS_WIDTH_1_BIT);

  if (priv->slot == 0 && wide)
    {
      esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D3,
                     INPUT | OUTPUT | PULLUP | DRIVE_3);
      gpio_ll_func_sel(GPIO_LL_GET_HW(0),
                       SDMMC_SLOT0_IOMUX_PIN_NUM_D3,
                       SDMMC_LL_IOMUX_FUNC);
    }

  priv->wide = wide;
}

static void esp32p4_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  uint32_t freq;
  uint32_t hostdiv;
  uint32_t carddiv;
  bool enable = true;

  switch (rate)
    {
      default:
      case CLOCK_SDIO_DISABLED:
        freq = 400000;
        enable = false;
        break;
      case CLOCK_IDMODE:
        freq = 400000;
        break;
      case CLOCK_MMC_TRANSFER:
        freq = 40000000;
        break;
      case CLOCK_SD_TRANSFER_4BIT:
        freq = 20000000;
        esp32p4_widebus(dev, true);
        break;
      case CLOCK_SD_TRANSFER_1BIT:
        freq = 20000000;
        esp32p4_widebus(dev, false);
        break;
    }

  sdmmc_ll_enable_card_clock(g_sdmmchost.hw, priv->slot, false);
  sdmmc_ll_enable_card_clock_low_power(g_sdmmchost.hw, priv->slot, false);
  if (esp32p4_clock_update(priv) < 0)
    {
      mcerr("ERROR: failed to disable card clock\n");
      return;
    }

  /* Keep the shared host clock at 80 MHz and use the independent card
   * dividers.  This lets slots 0 and 1 retain different bus frequencies.
   */

  hostdiv = 2;
  carddiv = (ESP32P4_SDMMC_SRC_FREQ / hostdiv) / (2 * freq);

  PERIPH_RCC_ATOMIC()
    {
      sdmmc_ll_select_clk_source(g_sdmmchost.hw, SDMMC_CLK_SRC_PLL160M);
      sdmmc_ll_set_clock_div(g_sdmmchost.hw, hostdiv);
      sdmmc_ll_init_phase_delay(g_sdmmchost.hw);
      sdmmc_ll_set_din_delay_phase(g_sdmmchost.hw,
                                   SDMMC_LL_DELAY_PHASE_0,
                                   SDMMC_LL_SPEED_MODE_LS);
    }

  sdmmc_ll_set_card_clock_div(g_sdmmchost.hw, priv->slot, carddiv);
  if (esp32p4_clock_update(priv) < 0)
    {
      mcerr("ERROR: failed to update card clock divisor\n");
      return;
    }

  if (enable)
    {
      sdmmc_ll_enable_card_clock(g_sdmmchost.hw, priv->slot, true);
      sdmmc_ll_enable_card_clock_low_power(g_sdmmchost.hw,
                                           priv->slot, true);
      if (esp32p4_clock_update(priv) < 0)
        {
          mcerr("ERROR: failed to enable card clock\n");
          return;
        }
    }

  sdmmc_ll_set_data_timeout(g_sdmmchost.hw, 0xffffff);
  sdmmc_ll_set_response_timeout(g_sdmmchost.hw, 0xff);
  priv->rate = rate;
}

static int esp32p4_attach(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  int ret;

  if (!g_sdmmchost.attached)
    {
      sdmmc_ll_enable_global_interrupt(g_sdmmchost.hw, false);
      sdmmc_ll_enable_interrupt(g_sdmmchost.hw,
                                SDMMC_LL_SD_EVENT_MASK, false);
      sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDMMC_LL_SD_EVENT_MASK);
      sdmmc_ll_clear_idsts_interrupt(g_sdmmchost.hw, UINT32_MAX);

      ret = esp_setup_irq(SDIO_HOST_INTR_SOURCE,
                          ESP_IRQ_PRIORITY_DEFAULT,
                          ESP_IRQ_TRIGGER_LEVEL,
                          esp32p4_interrupt, NULL);
      if (ret >= 0)
        {
          g_sdmmchost.attached = true;
          esp32p4_enable_ints(priv);
          sdmmc_ll_enable_global_interrupt(g_sdmmchost.hw, true);
          up_enable_irq(ESP_IRQ_SDIO_HOST);
          ret = OK;
        }
    }
  else
    {
      ret = OK;
    }

  return ret;
}

static int esp32p4_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t arg)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  sdmmc_hw_cmd_t command =
  {
    0
  };
  uint32_t response = cmd & MMCSD_RESPONSE_MASK;
  uint32_t xfr = cmd & MMCSD_DATAXFR_MASK;
  int ret;

  if (!g_sdmmchost.resetok)
    {
      mcerr("ERROR: slot %d command rejected: controller reset failed\n",
            priv->slot);
      return -ENODEV;
    }

  command.cmd_index = (cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT;
  command.wait_complete = 1;
  if (command.cmd_index == MMCSD_CMDIDX0)
    {
      command.send_init = 1;
    }

  if ((cmd & MMCSD_STOPXFR) != 0)
    {
      command.stop_abort_cmd = 1;
      command.wait_complete = 0;
    }

  if (xfr != MMCSD_NODATAXFR)
    {
      command.data_expected = 1;
      command.transfer_mode = xfr == MMCSD_RDSTREAM ||
                              xfr == MMCSD_WRSTREAM;
      command.rw = xfr == MMCSD_WRDATAXFR || xfr == MMCSD_WRSTREAM;
      command.send_auto_stop = (cmd & MMCSD_MULTIBLOCK) != 0;
    }

  if (response != MMCSD_NO_RESPONSE)
    {
      command.response_expect = 1;
      command.response_long = response == MMCSD_R2_RESPONSE;
      command.check_response_crc = response != MMCSD_R3_RESPONSE &&
                                   response != MMCSD_R4_RESPONSE;
    }

  g_sdmmchost.active = priv;
  priv->cmdstatus = 0;
  nxsem_reset(&priv->cmdsem, 0);
  sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDCARD_CMD_MASK);
  esp32p4_enable_ints(priv);
  ret = esp32p4_ciu_sendcmd(priv, command, arg);
  if (ret < 0)
    {
      mcerr("ERROR: slot %d failed to submit cmd=%u: %d\n",
            priv->slot, command.cmd_index, ret);
    }

  return ret;
}

#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp32p4_blocksetup(struct sdio_dev_s *dev,
                               unsigned int blocklen,
                               unsigned int nblocks)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  DEBUGASSERT(blocklen > 0 && nblocks > 0);
  priv->blocklen = blocklen;
  priv->nblocks = nblocks;
  sdmmc_ll_set_block_size(g_sdmmchost.hw, blocklen);
  sdmmc_ll_set_data_transfer_len(g_sdmmchost.hw, blocklen * nblocks);
}
#endif

static int esp32p4_setup_dma(struct esp32p4_dev_s *priv,
                             uint8_t *buffer, size_t nbytes, bool write)
{
  size_t allocsize;
  size_t remaining;
  size_t offset;
  size_t i;

  if (buffer == NULL || nbytes == 0 || nbytes > ESP32P4_SDMMC_MAX_XFR)
    {
      return -EINVAL;
    }

  esp32p4_release_buffer(priv);
  allocsize = (nbytes + ESP32P4_SDMMC_CACHE_ALIGN - 1) &
              ~(ESP32P4_SDMMC_CACHE_ALIGN - 1);
  priv->dmabuffer = kmm_memalign(ESP32P4_SDMMC_CACHE_ALIGN, allocsize);
  if (priv->dmabuffer == NULL)
    {
      return -ENOMEM;
    }

  priv->buffer = buffer;
  priv->remaining = nbytes;
  priv->wrdir = write;
  priv->xfrdone = false;
  priv->dmadone = false;
  if (write)
    {
      memcpy(priv->dmabuffer, buffer, nbytes);
      if (esp32p4_cache_sync(priv, priv->dmabuffer, allocsize,
                             ESP_CACHE_MSYNC_FLAG_DIR_C2M,
                             "TX buffer") < 0)
        {
          priv->remaining = 0;
          esp32p4_release_buffer(priv);
          return -EIO;
        }
    }
  else
    {
      /* Discard any cache lines for the destination before IDMAC writes it,
       * so a later writeback cannot overwrite the received data.
       */

      if (esp32p4_cache_sync(priv, priv->dmabuffer, allocsize,
                             ESP_CACHE_MSYNC_FLAG_DIR_M2C,
                             "RX buffer") < 0)
        {
          priv->remaining = 0;
          esp32p4_release_buffer(priv);
          return -EIO;
        }
    }

  memset(priv->dma_desc, 0, sizeof(priv->dma_desc));
  remaining = nbytes;
  offset = 0;
  for (i = 0; remaining != 0; i++)
    {
      size_t len = remaining > SDMMC_DMA_MAX_BUF_LEN ?
                   SDMMC_DMA_MAX_BUF_LEN : remaining;

      DEBUGASSERT(i < ESP32P4_SDMMC_NDESCS);
      priv->dma_desc[i].owned_by_idmac = 1;
      priv->dma_desc[i].second_address_chained = 1;
      priv->dma_desc[i].first_descriptor = i == 0;
      priv->dma_desc[i].last_descriptor = len == remaining;
      priv->dma_desc[i].disable_int_on_completion = len != remaining;
      priv->dma_desc[i].buffer1_size = (len + 3) & ~3;
      priv->dma_desc[i].buffer1_ptr = priv->dmabuffer + offset;
      priv->dma_desc[i].next_desc_ptr = len == remaining ? NULL :
                                        &priv->dma_desc[i + 1];
      remaining -= len;
      offset += len;
    }

  if (esp32p4_cache_sync(priv, priv->dma_desc, sizeof(priv->dma_desc),
                         ESP_CACHE_MSYNC_FLAG_DIR_C2M,
                         "DMA descriptor") < 0)
    {
      priv->remaining = 0;
      esp32p4_release_buffer(priv);
      return -EIO;
    }
  if (priv->blocklen == 0 || priv->blocklen * priv->nblocks != nbytes)
    {
      priv->blocklen = nbytes > 512 ? 512 : nbytes;
      priv->nblocks = (nbytes + priv->blocklen - 1) / priv->blocklen;
    }

  sdmmc_ll_set_block_size(g_sdmmchost.hw, priv->blocklen);
  sdmmc_ll_set_data_transfer_len(g_sdmmchost.hw, nbytes);
  sdmmc_ll_set_desc_addr(g_sdmmchost.hw,
                         (uint32_t)(uintptr_t)priv->dma_desc);
  sdmmc_ll_clear_interrupt(g_sdmmchost.hw, SDCARD_XFR_MASK);
  sdmmc_ll_clear_idsts_interrupt(g_sdmmchost.hw, UINT32_MAX);
  sdmmc_ll_enable_dma(g_sdmmchost.hw, true);
  sdmmc_ll_poll_demand(g_sdmmchost.hw);
  g_sdmmchost.active = priv;
  esp32p4_enable_ints(priv);
  return OK;
}

static int esp32p4_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                             size_t nbytes)
{
  return esp32p4_setup_dma((struct esp32p4_dev_s *)dev, buffer,
                           nbytes, false);
}

static int esp32p4_sendsetup(struct sdio_dev_s *dev,
                             const uint8_t *buffer, size_t nbytes)
{
  return esp32p4_setup_dma((struct esp32p4_dev_s *)dev,
                           (uint8_t *)(uintptr_t)buffer, nbytes, true);
}

static int esp32p4_cancel(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  sdmmc_ll_stop_dma(g_sdmmchost.hw);
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, SDCARD_XFR_MASK, false);
  sdmmc_ll_clear_interrupt(g_sdmmchost.hw,
                           SDCARD_XFR_MASK | SDCARD_CMD_MASK);
  sdmmc_ll_clear_idsts_interrupt(g_sdmmchost.hw, UINT32_MAX);
  wd_cancel(&priv->waitwdog);
  priv->waitevents = 0;
  priv->waitmask = 0;
  priv->iowait = false;
  priv->wkupevent = 0;
  priv->remaining = 0;
  priv->xfrdone = false;
  priv->dmadone = false;
  esp32p4_release_buffer(priv);
  return OK;
}

static int esp32p4_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  uint32_t response = cmd & MMCSD_RESPONSE_MASK;
  clock_t timeout;
  clock_t start;
  int ret;

  switch (response)
    {
      case MMCSD_NO_RESPONSE:
      case MMCSD_R3_RESPONSE:
      case MMCSD_R7_RESPONSE:
        timeout = SDCARD_CMDTIMEOUT;
        break;
      case MMCSD_R1_RESPONSE:
      case MMCSD_R1B_RESPONSE:
      case MMCSD_R2_RESPONSE:
      case MMCSD_R4_RESPONSE:
      case MMCSD_R5_RESPONSE:
      case MMCSD_R6_RESPONSE:
        timeout = SDCARD_LONGTIMEOUT;
        break;
      default:
        return -EINVAL;
    }

  ret = nxsem_tickwait_uninterruptible(&priv->cmdsem, timeout);
  if (ret < 0)
    {
      mcerr("ERROR: slot %d command wait failed: %d\n", priv->slot, ret);
      return ret == -ETIMEDOUT ? ret : -EIO;
    }

  if ((priv->cmdstatus & (SDMMC_LL_EVENT_RTO |
                          SDMMC_LL_EVENT_HLE)) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((priv->cmdstatus & (SDMMC_LL_EVENT_RESP_ERR |
                          SDMMC_LL_EVENT_RCRC)) != 0)
    {
      return -EIO;
    }

  if (response == MMCSD_R1B_RESPONSE)
    {
      start = clock_systime_ticks();
      while (sdmmc_ll_is_card_data_busy(g_sdmmchost.hw))
        {
          if (clock_systime_ticks() - start > SDCARD_LONGTIMEOUT)
            {
              return -ETIMEDOUT;
            }
        }
    }

  return OK;
}

static int esp32p4_response_error(struct esp32p4_dev_s *priv, bool crc)
{
  if ((priv->cmdstatus & (SDMMC_LL_EVENT_RTO |
                          SDMMC_LL_EVENT_HLE)) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((priv->cmdstatus & SDMMC_LL_EVENT_RESP_ERR) != 0 ||
      (crc && (priv->cmdstatus & SDMMC_LL_EVENT_RCRC) != 0))
    {
      return -EIO;
    }

  if ((priv->cmdstatus & SDMMC_LL_EVENT_CMD_DONE) == 0)
    {
      return -EIO;
    }

  return OK;
}

static int esp32p4_recvshortcrc(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t *rshort)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  (void)cmd;

  if (rshort == NULL)
    {
      return -EINVAL;
    }

  *rshort = g_sdmmchost.hw->resp[0];
  return esp32p4_response_error(priv, true);
}

static int esp32p4_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t rlong[4])
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  (void)cmd;

  if (rlong == NULL)
    {
      return -EINVAL;
    }

  rlong[0] = g_sdmmchost.hw->resp[3];
  rlong[1] = g_sdmmchost.hw->resp[2];
  rlong[2] = g_sdmmchost.hw->resp[1];
  rlong[3] = g_sdmmchost.hw->resp[0];
  return esp32p4_response_error(priv, true);
}

static int esp32p4_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                             uint32_t *rshort)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  (void)cmd;

  if (rshort == NULL)
    {
      return -EINVAL;
    }

  *rshort = g_sdmmchost.hw->resp[0];
  return esp32p4_response_error(priv, false);
}

static void esp32p4_waitenable(struct sdio_dev_s *dev,
                               sdio_eventset_t eventset,
                               uint32_t timeout)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  irqstate_t flags;

  wd_cancel(&priv->waitwdog);
  nxsem_reset(&priv->waitsem, 0);
  flags = enter_critical_section();
  priv->waitevents = eventset;
  priv->wkupevent = 0;
  priv->waitmask = 0;
  priv->iowait = false;
  if ((eventset & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
    {
      priv->waitmask |= SDCARD_CMD_MASK;
    }

  esp32p4_enable_ints(priv);
  leave_critical_section(flags);

  if ((eventset & SDIOWAIT_TIMEOUT) != 0)
    {
      if (timeout == 0)
        {
          flags = enter_critical_section();
          esp32p4_endwait(priv, SDIOWAIT_TIMEOUT);
          leave_critical_section(flags);
        }
      else if (wd_start(&priv->waitwdog, MSEC2TICK(timeout),
                        esp32p4_eventtimeout,
                        (wdparm_t)(uintptr_t)priv) < 0)
        {
          mcerr("ERROR: failed to start SDMMC timeout watchdog\n");
        }
    }
}

static sdio_eventset_t esp32p4_eventwait(struct sdio_dev_s *dev)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  sdio_eventset_t event;
  size_t nbytes = priv->remaining;
  size_t syncsize = (nbytes + ESP32P4_SDMMC_CACHE_ALIGN - 1) &
                    ~(ESP32P4_SDMMC_CACHE_ALIGN - 1);
  int ret;

  if (priv->waitevents == 0 && priv->wkupevent == 0)
    {
      irqstate_t flags = enter_critical_section();

      priv->iowait = true;
      priv->waitevents = SDIOWAIT_CMDDONE;
      priv->waitmask = SDMMC_LL_EVENT_IO_SLOT0 << priv->slot;
      esp32p4_enable_ints(priv);
      leave_critical_section(flags);
    }

  do
    {
      ret = nxsem_wait_uninterruptible(&priv->waitsem);
      if (ret < 0)
        {
          wd_cancel(&priv->waitwdog);
          event = SDIOWAIT_ERROR;
          goto out;
        }

      event = priv->wkupevent;
    }
  while (event == 0);

out:
  if ((event & SDIOWAIT_TRANSFERDONE) != 0)
    {
      if ((event & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) == 0 &&
          !priv->wrdir && priv->dmabuffer != NULL)
        {
          if (esp32p4_cache_sync(priv, priv->dmabuffer, syncsize,
                                 ESP_CACHE_MSYNC_FLAG_DIR_M2C,
                                 "RX buffer") < 0)
            {
              event |= SDIOWAIT_ERROR;
            }
          else
            {
              memcpy(priv->buffer, priv->dmabuffer, nbytes);
            }
        }

      priv->remaining = 0;
      esp32p4_release_buffer(priv);
    }

  priv->waitevents = 0;
  priv->waitmask = 0;
  priv->iowait = false;
  priv->wkupevent = 0;
  esp32p4_enable_ints(priv);
  return event;
}

static void esp32p4_callbackenable(struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  priv->cbevents = eventset;
  esp32p4_status(dev);
  esp32p4_callback(priv);
}

static int esp32p4_registercallback(struct sdio_dev_s *dev,
                                    worker_t callback, void *arg)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;

  priv->cbevents = 0;
  priv->callback = callback;
  priv->cbarg = arg;
  return OK;
}

static void esp32p4_configure_slot0(void)
{
  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_CLK,
                 OUTPUT | FUNCTION | DRIVE_3);
  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_CMD,
                 INPUT | OUTPUT | FUNCTION | PULLUP | DRIVE_3);
  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D0,
                 INPUT | OUTPUT | FUNCTION | PULLUP | DRIVE_3);
  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D1,
                 INPUT | OUTPUT | FUNCTION | PULLUP | DRIVE_3);
  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D2,
                 INPUT | OUTPUT | FUNCTION | PULLUP | DRIVE_3);
  /* DAT3 must remain high when CMD0 is issued, otherwise an SD card can
   * enter SPI mode and stop responding to native SD commands.
   */

  esp_configgpio(SDMMC_SLOT0_IOMUX_PIN_NUM_D3,
                 OUTPUT | PULLUP | DRIVE_3);
  esp_gpiowrite(SDMMC_SLOT0_IOMUX_PIN_NUM_D3, true);

  gpio_ll_func_sel(GPIO_LL_GET_HW(0), SDMMC_SLOT0_IOMUX_PIN_NUM_CLK,
                   SDMMC_LL_IOMUX_FUNC);
  gpio_ll_func_sel(GPIO_LL_GET_HW(0), SDMMC_SLOT0_IOMUX_PIN_NUM_CMD,
                   SDMMC_LL_IOMUX_FUNC);
  gpio_ll_func_sel(GPIO_LL_GET_HW(0), SDMMC_SLOT0_IOMUX_PIN_NUM_D0,
                   SDMMC_LL_IOMUX_FUNC);
  gpio_ll_func_sel(GPIO_LL_GET_HW(0), SDMMC_SLOT0_IOMUX_PIN_NUM_D1,
                   SDMMC_LL_IOMUX_FUNC);
  gpio_ll_func_sel(GPIO_LL_GET_HW(0), SDMMC_SLOT0_IOMUX_PIN_NUM_D2,
                   SDMMC_LL_IOMUX_FUNC);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_sdio_wait_card_interrupt(struct sdio_dev_s *dev,
                                     uint32_t timeout_ticks)
{
  struct esp32p4_dev_s *priv = (struct esp32p4_dev_s *)dev;
  uint32_t iomask;
  irqstate_t flags;
  int d1pin;
  int ret;

  if (priv == NULL || priv->slot < 0 ||
      priv->slot >= ESP32P4_SDMMC_NSLOTS)
    {
      return -EINVAL;
    }

  if (priv->slot == 0)
    {
      d1pin = SDMMC_SLOT0_IOMUX_PIN_NUM_D1;
    }
#ifdef CONFIG_ESP32P4_SDMMC_SLOT1
  else
    {
      d1pin = CONFIG_ESP32P4_SDMMC_SLOT1_PIN_D1;
    }
#else
  else
    {
      return -ENODEV;
    }
#endif

  iomask = SDMMC_LL_EVENT_IO_SLOT0 << priv->slot;
  nxsem_reset(&priv->iosem, 0);

  /* SDIO function interrupts are negative-edge sensitive.  Disable and
   * clear the source first, then sample DAT1.  If DAT1 is already low the
   * edge happened before the wait was armed and data is pending.  Otherwise
   * a subsequent falling edge is guaranteed to wake the dedicated waiter.
   */

  flags = enter_critical_section();
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, iomask, false);
  sdmmc_ll_clear_interrupt(g_sdmmchost.hw, iomask);

  if (!esp_gpioread(d1pin))
    {
      leave_critical_section(flags);
      return OK;
    }

  priv->ioirqwait = true;
  esp32p4_enable_ints(priv);
  leave_critical_section(flags);

  if (timeout_ticks == UINT32_MAX)
    {
      ret = nxsem_wait_uninterruptible(&priv->iosem);
    }
  else
    {
      ret = nxsem_tickwait_uninterruptible(&priv->iosem, timeout_ticks);
    }

  flags = enter_critical_section();
  priv->ioirqwait = false;
  sdmmc_ll_enable_interrupt(g_sdmmchost.hw, iomask, false);
  leave_critical_section(flags);
  return ret;
}

struct sdio_dev_s *sdio_initialize(int slotno)
{
  struct esp32p4_dev_s *priv;

  if (slotno < 0 || slotno >= ESP32P4_SDMMC_NSLOTS)
    {
      return NULL;
    }

#ifndef CONFIG_ESP32P4_SDMMC_SLOT0
  if (slotno == 0)
    {
      return NULL;
    }
#endif

#ifndef CONFIG_ESP32P4_SDMMC_SLOT1
  if (slotno == 1)
    {
      return NULL;
    }
#endif

  priv = &g_sdiodev[slotno];
  if (!g_sdmmchost.clocked)
    {
      PERIPH_RCC_ATOMIC()
        {
          sdmmc_ll_enable_bus_clock(0, true);
          sdmmc_ll_reset_register(0);
          sdmmc_ll_select_clk_source(g_sdmmchost.hw,
                                     SDMMC_CLK_SRC_PLL160M);
          sdmmc_ll_set_clock_div(g_sdmmchost.hw, 2);
          sdmmc_ll_init_phase_delay(g_sdmmchost.hw);
          sdmmc_ll_set_din_delay_phase(g_sdmmchost.hw,
                                       SDMMC_LL_DELAY_PHASE_0,
                                       SDMMC_LL_SPEED_MODE_LS);
        }

      up_udelay(10);
      g_sdmmchost.clocked = true;
    }

  if (slotno == 0)
    {
      esp32p4_configure_slot0();
    }

  esp32p4_reset(&priv->dev);
  if (!g_sdmmchost.resetok)
    {
      mcerr("ERROR: sdio_initialize(%d): controller reset failed\n", slotno);
      return NULL;
    }

  priv->inited = true;
  esp_gpio_matrix_in(GPIO_MATRIX_CONST_ONE_INPUT,
                     sdmmc_slot_info[slotno].card_int, false);
  esp_gpio_matrix_in(GPIO_MATRIX_CONST_ZERO_INPUT,
                     sdmmc_slot_info[slotno].card_detect, false);
  esp_gpio_matrix_in(GPIO_MATRIX_CONST_ONE_INPUT,
                     sdmmc_slot_info[slotno].write_protect, true);

  return &priv->dev;
}

#endif /* CONFIG_ESP32P4_SDMMC */
