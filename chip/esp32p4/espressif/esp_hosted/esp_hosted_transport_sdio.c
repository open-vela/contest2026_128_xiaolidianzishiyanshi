/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_hosted_transport_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/sdio.h>
#include <nuttx/kmalloc.h>

#include "esp_hosted_port.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_transport_config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDIO_BLOCK_SIZE     512
#define SDIO_INIT_MAX_RETRY 10

#define SDIO_FUNC_0         0
#define SDIO_FUNC_1         1

/* SDIO CCCR registers */

#define SD_IO_CCCR_FN_ENABLE    0x02
#define SD_IO_CCCR_FN_READY     0x03
#define SD_IO_CCCR_INT_ENABLE   0x04
#define SD_IO_CCCR_BUS_WIDTH    0x07

/* SDIO FBR registers */

#define SD_IO_FBR_START         0x100

/* SDIO block size registers */

#define SD_IO_CCCR_BLKSIZEL     0x10
#define SD_IO_CCCR_BLKSIZEH     0x11

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hosted_sdio_context {
    struct sdio_dev_s *sdio_dev;
    uint32_t clock_freq_khz;
    uint8_t bus_width;
    uint8_t slot;
    mutex_t lock;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct hosted_sdio_context *g_sdio_ctx = NULL;

/****************************************************************************
 * Private Function Prototypes (forward declarations)
 ****************************************************************************/

void *hosted_sdio_bus_init(void);
int hosted_sdio_bus_deinit(void *ctx);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hosted_sdio_set_blocksize
 *
 * Description:
 *   Set SDIO function block size.
 *
 ****************************************************************************/

static int hosted_sdio_set_blocksize(struct sdio_dev_s *dev, uint8_t fn,
                                     uint16_t size)
{
    uint8_t lo = size & 0xff;
    uint8_t hi = (size >> 8) & 0xff;
    uint16_t offset = SD_IO_FBR_START * fn;
    int ret;

    ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_0,
                         offset + SD_IO_CCCR_BLKSIZEL, &lo, 1);
    if (ret < 0) {
        return ret;
    }

    ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_0,
                         offset + SD_IO_CCCR_BLKSIZEH, &hi, 1);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/****************************************************************************
 * Name: hosted_sdio_card_fn_init
 *
 * Description:
 *   Initialize SDIO card function 1.
 *
 ****************************************************************************/

static int hosted_sdio_card_fn_init(struct sdio_dev_s *dev)
{
    uint8_t ioe = 0;
    uint8_t ior = 0;
    uint8_t ie = 0;
    int i;
    int ret;

    /* Enable function 1 */

    ret = SDIO_READFUNC(dev, SDIO_FUNC_0, SD_IO_CCCR_FN_ENABLE, &ioe, 1);
    if (ret < 0) {
        return ret;
    }

    wlinfo("IOE: 0x%02x\n", ioe);

    ioe |= (1 << 1);  /* FUNC1_EN_MASK */
    ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_0, SD_IO_CCCR_FN_ENABLE, &ioe, 1);
    if (ret < 0) {
        return ret;
    }

    /* Wait for card to become ready */

    for (i = 0; i < SDIO_INIT_MAX_RETRY; i++) {
        ret = SDIO_READFUNC(dev, SDIO_FUNC_0, SD_IO_CCCR_FN_READY, &ior, 1);
        if (ret < 0) {
            return ret;
        }

        wldebug("IOR: 0x%02x\n", ior);
        if (ior & (1 << 1)) {
            break;
        }

        usleep(10 * 1000);
    }

    if (i >= SDIO_INIT_MAX_RETRY) {
        wlerr("ERROR: SDIO card failed to become ready\n");
        return -ETIMEDOUT;
    }

    /* Enable interrupts for function 1 and master enable */

    ret = SDIO_READFUNC(dev, SDIO_FUNC_0, SD_IO_CCCR_INT_ENABLE, &ie, 1);
    if (ret < 0) {
        return ret;
    }

    wldebug("IE: 0x%02x\n", ie);

    ie |= (1 << 0) | (1 << 1);  /* Master enable + FUNC1 */
    ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_0, SD_IO_CCCR_INT_ENABLE, &ie, 1);
    if (ret < 0) {
        return ret;
    }

    /* Set FN0 and FN1 block size to 512 */

    ret = hosted_sdio_set_blocksize(dev, SDIO_FUNC_0, SDIO_BLOCK_SIZE);
    if (ret < 0) {
        return ret;
    }

    ret = hosted_sdio_set_blocksize(dev, SDIO_FUNC_1, SDIO_BLOCK_SIZE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/****************************************************************************
 * Public Functions (OSI Interface)
 ****************************************************************************/

/****************************************************************************
 * Name: hosted_sdio_card_init
 *
 * Description:
 *   Initialize SDIO card.
 *
 ****************************************************************************/

int hosted_sdio_card_init(void *ctx, bool show_config)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;
    int ret;

    if (!context || !context->sdio_dev) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    /* Initialize SDIO bus */

    ret = SDIO_SETFREQUENCY(dev, context->clock_freq_khz * 1000);
    if (ret < 0) {
        wlerr("ERROR: Failed to set SDIO frequency: %d\n", ret);
        return ret;
    }

    if (context->bus_width == 4) {
        ret = SDIO_SETBUSWIDTH(dev, 4);
        if (ret < 0) {
            wlerr("ERROR: Failed to set 4-bit bus width: %d\n", ret);
            return ret;
        }
    }

    /* Initialize card function */

    ret = hosted_sdio_card_fn_init(dev);
    if (ret < 0) {
        wlerr("ERROR: Failed to initialize SDIO card function: %d\n", ret);
        return ret;
    }

    wlinfo("SDIO card initialized successfully\n");
    return 0;
}

/****************************************************************************
 * Name: hosted_sdio_card_deinit
 *
 * Description:
 *   Deinitialize SDIO card.
 *
 ****************************************************************************/

int hosted_sdio_card_deinit(void *ctx)
{
    /* Nothing specific to deinit for SDIO */

    return 0;
}

/****************************************************************************
 * Name: hosted_sdio_read_reg
 *
 * Description:
 *   Read SDIO register.
 *
 ****************************************************************************/

int hosted_sdio_read_reg(void *ctx, uint32_t reg, uint8_t *data,
                         uint16_t size, bool lock_required)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;
    int ret;

    if (!context || !context->sdio_dev || !data) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    if (lock_required) {
        nxmutex_lock(&context->lock);
    }

    ret = SDIO_READFUNC(dev, SDIO_FUNC_1, reg, data, size);

    if (lock_required) {
        nxmutex_unlock(&context->lock);
    }

    return ret;
}

/****************************************************************************
 * Name: hosted_sdio_write_reg
 *
 * Description:
 *   Write SDIO register.
 *
 ****************************************************************************/

int hosted_sdio_write_reg(void *ctx, uint32_t reg, uint8_t *data,
                          uint16_t size, bool lock_required)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;
    int ret;

    if (!context || !context->sdio_dev || !data) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    if (lock_required) {
        nxmutex_lock(&context->lock);
    }

    ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_1, reg, data, size);

    if (lock_required) {
        nxmutex_unlock(&context->lock);
    }

    return ret;
}

/****************************************************************************
 * Name: hosted_sdio_read_block
 *
 * Description:
 *   Read SDIO block.
 *
 ****************************************************************************/

int hosted_sdio_read_block(void *ctx, uint32_t reg, uint8_t *data,
                           uint16_t size, bool lock_required)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;
    int ret;

    if (!context || !context->sdio_dev || !data) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    if (lock_required) {
        nxmutex_lock(&context->lock);
    }

    /* Use block mode transfer */

    uint16_t blocks = size / SDIO_BLOCK_SIZE;
    uint16_t remainder = size % SDIO_BLOCK_SIZE;

    if (blocks > 0) {
        ret = SDIO_READBLOCK(dev, reg, data, blocks);
        if (ret < 0) {
            goto errout;
        }
    }

    if (remainder > 0) {
        /* Read remainder using byte mode */

        ret = SDIO_READFUNC(dev, SDIO_FUNC_1,
                            reg + blocks * SDIO_BLOCK_SIZE,
                            data + blocks * SDIO_BLOCK_SIZE,
                            remainder);
        if (ret < 0) {
            goto errout;
        }
    }

    ret = 0;

errout:
    if (lock_required) {
        nxmutex_unlock(&context->lock);
    }

    return ret;
}

/****************************************************************************
 * Name: hosted_sdio_write_block
 *
 * Description:
 *   Write SDIO block.
 *
 ****************************************************************************/

int hosted_sdio_write_block(void *ctx, uint32_t reg, uint8_t *data,
                            uint16_t size, bool lock_required)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;
    int ret;

    if (!context || !context->sdio_dev || !data) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    if (lock_required) {
        nxmutex_lock(&context->lock);
    }

    /* Use block mode transfer */

    uint16_t blocks = size / SDIO_BLOCK_SIZE;
    uint16_t remainder = size % SDIO_BLOCK_SIZE;

    if (blocks > 0) {
        ret = SDIO_WRITEBLOCK(dev, reg, data, blocks);
        if (ret < 0) {
            goto errout;
        }
    }

    if (remainder > 0) {
        /* Write remainder using byte mode */

        ret = SDIO_WRITEFUNC(dev, SDIO_FUNC_1,
                             reg + blocks * SDIO_BLOCK_SIZE,
                             data + blocks * SDIO_BLOCK_SIZE,
                             remainder);
        if (ret < 0) {
            goto errout;
        }
    }

    ret = 0;

errout:
    if (lock_required) {
        nxmutex_unlock(&context->lock);
    }

    return ret;
}

/****************************************************************************
 * Name: hosted_sdio_wait_slave_intr
 *
 * Description:
 *   Wait for slave interrupt.
 *
 ****************************************************************************/

int hosted_sdio_wait_slave_intr(void *ctx, uint32_t ticks_to_wait)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;
    struct sdio_dev_s *dev;

    if (!context || !context->sdio_dev) {
        return -EINVAL;
    }

    dev = context->sdio_dev;

    /* Wait for SDIO interrupt from slave */

    return SDIO_WAITINT(dev, ticks_to_wait);
}

/****************************************************************************
 * Name: hosted_sdio_init
 *
 * Description:
 *   Initialize SDIO transport and register OSI functions.
 *
 ****************************************************************************/

int hosted_sdio_init(void)
{
    struct sdio_dev_s *sdio_dev;

    /* Allocate SDIO context */

    g_sdio_ctx = (struct hosted_sdio_context *)kumm_malloc(
                     sizeof(struct hosted_sdio_context));
    if (!g_sdio_ctx) {
        return -ENOMEM;
    }

    memset(g_sdio_ctx, 0, sizeof(struct hosted_sdio_context));

    /* Initialize SDIO device */

    sdio_dev = sdio_initialize(CONFIG_ESP_HOSTED_SDIO_SLOT);
    if (!sdio_dev) {
        wlerr("ERROR: Failed to initialize SDIO slot %d\n",
              CONFIG_ESP_HOSTED_SDIO_SLOT);
        kumm_free(g_sdio_ctx);
        g_sdio_ctx = NULL;
        return -ENODEV;
    }

    g_sdio_ctx->sdio_dev = sdio_dev;
    g_sdio_ctx->clock_freq_khz = CONFIG_ESP_HOSTED_SDIO_FREQ_KHZ;
    g_sdio_ctx->bus_width = CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH_4BIT ? 4 : 1;
    g_sdio_ctx->slot = CONFIG_ESP_HOSTED_SDIO_SLOT;

    nxmutex_init(&g_sdio_ctx->lock);

    /* Register SDIO functions in OSI */

    g_h.funcs->_h_sdio_card_init = hosted_sdio_card_init;
    g_h.funcs->_h_sdio_card_deinit = hosted_sdio_card_deinit;
    g_h.funcs->_h_sdio_read_reg = hosted_sdio_read_reg;
    g_h.funcs->_h_sdio_write_reg = hosted_sdio_write_reg;
    g_h.funcs->_h_sdio_read_block = hosted_sdio_read_block;
    g_h.funcs->_h_sdio_write_block = hosted_sdio_write_block;
    g_h.funcs->_h_sdio_wait_slave_intr = hosted_sdio_wait_slave_intr;

    /* Set bus init/deinit */

    g_h.funcs->_h_bus_init = hosted_sdio_bus_init;
    g_h.funcs->_h_bus_deinit = hosted_sdio_bus_deinit;

    wlinfo("SDIO transport initialized\n");
    return 0;
}

/****************************************************************************
 * Name: hosted_sdio_bus_init
 *
 * Description:
 *   Initialize SDIO bus (called by esp_hosted_init).
 *
 ****************************************************************************/

void *hosted_sdio_bus_init(void)
{
    int ret;

    if (!g_sdio_ctx) {
        ret = hosted_sdio_init();
        if (ret < 0) {
            return NULL;
        }
    }

    /* Initialize card */

    ret = hosted_sdio_card_init(g_sdio_ctx, true);
    if (ret < 0) {
        wlerr("ERROR: SDIO card init failed: %d\n", ret);
        return NULL;
    }

    return g_sdio_ctx;
}

/****************************************************************************
 * Name: hosted_sdio_bus_deinit
 *
 * Description:
 *   Deinitialize SDIO bus.
 *
 ****************************************************************************/

int hosted_sdio_bus_deinit(void *ctx)
{
    struct hosted_sdio_context *context = (struct hosted_sdio_context *)ctx;

    if (context) {
        hosted_sdio_card_deinit(context);
        nxmutex_destroy(&context->lock);
        kumm_free(context);
        g_sdio_ctx = NULL;
    }

    return 0;
}
