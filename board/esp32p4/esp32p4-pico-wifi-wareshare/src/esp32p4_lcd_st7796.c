/****************************************************************************
 * board/esp32p4/esp32p4-pico-wifi-wareshare/src/
 * esp32p4_lcd_st7796.c
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
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/spi/spi.h>
#include <nuttx/video/fb.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_spi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ST7796_BPP                16
#define ST7796_BYTES_PER_PIXEL    2

#define ST7796_SWRESET            0x01
#define ST7796_SLPIN              0x10
#define ST7796_SLPOUT             0x11
#define ST7796_INVON              0x21
#define ST7796_DISPOFF            0x28
#define ST7796_DISPON             0x29
#define ST7796_CASET              0x2a
#define ST7796_RASET              0x2b
#define ST7796_RAMWR              0x2c
#define ST7796_MADCTL             0x36
#define ST7796_COLMOD             0x3a

/* The official BSP configures BGR color order then mirrors the X axis. */

#define ST7796_MADCTL_BGR         (1 << 3)
#define ST7796_MADCTL_MX          (1 << 6)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct st7796_initcmd_s
{
  uint8_t cmd;
  uint8_t len;
  uint16_t delay_ms;
  uint8_t data[14];
};

struct st7796_dev_s
{
  struct lcd_dev_s dev;
  struct spi_dev_s *spi;
  uint8_t power;
  uint16_t runbuffer[BOARD_LCD_WIDTH];
  uint8_t txbuffer[BOARD_LCD_WIDTH * ST7796_BYTES_PER_PIXEL];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int st7796_putrun(struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, const uint8_t *buffer,
                         size_t npixels);
static int st7796_putarea(struct lcd_dev_s *dev,
                          fb_coord_t row_start, fb_coord_t row_end,
                          fb_coord_t col_start, fb_coord_t col_end,
                          const uint8_t *buffer, fb_coord_t stride);
static int st7796_getvideoinfo(struct lcd_dev_s *dev,
                               struct fb_videoinfo_s *vinfo);
static int st7796_getplaneinfo(struct lcd_dev_s *dev, unsigned int planeno,
                               struct lcd_planeinfo_s *pinfo);
static int st7796_getpower(struct lcd_dev_s *dev);
static int st7796_setpower(struct lcd_dev_s *dev, int power);
static int st7796_getcontrast(struct lcd_dev_s *dev);
static int st7796_setcontrast(struct lcd_dev_s *dev,
                              unsigned int contrast);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Waveshare ESP32-S3-Touch-LCD-3.5 ST7796 panel initialization table.
 *
 * Keep the panel-specific power, VCOM and gamma values intact.  The table
 * deliberately contains SLPOUT and COLMOD even though the generic panel
 * preamble sends them too; this matches Waveshare's driver sequence.
 */

static const struct st7796_initcmd_s g_st7796_init[] =
{
  {
    .cmd      = ST7796_SLPOUT,
    .delay_ms = 120,
  },
  {
    .cmd  = ST7796_COLMOD,
    .len  = 1,
    .data =
      {
        0x05
      },
  },
  {
    .cmd  = 0xf0,
    .len  = 1,
    .data =
      {
        0xc3
      },
  },
  {
    .cmd  = 0xf0,
    .len  = 1,
    .data =
      {
        0x96
      },
  },
  {
    .cmd  = 0xb4,
    .len  = 1,
    .data =
      {
        0x01
      },
  },
  {
    .cmd  = 0xb7,
    .len  = 1,
    .data =
      {
        0xc6
      },
  },
  {
    .cmd  = 0xc0,
    .len  = 2,
    .data =
      {
        0x80, 0x45
      },
  },
  {
    .cmd  = 0xc1,
    .len  = 1,
    .data =
      {
        0x13
      },
  },
  {
    .cmd  = 0xc2,
    .len  = 1,
    .data =
      {
        0xa7
      },
  },
  {
    .cmd  = 0xc5,
    .len  = 1,
    .data =
      {
        0x0a
      },
  },
  {
    .cmd  = 0xe8,
    .len  = 8,
    .data =
      {
        0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33
      },
  },
  {
    .cmd = 0xe0,
    .len = 14,
    .data =
      {
        0xd0, 0x08, 0x0f, 0x06, 0x06, 0x33, 0x30,
        0x33, 0x47, 0x17, 0x13, 0x13, 0x2b, 0x31
      },
  },
  {
    .cmd = 0xe1,
    .len = 14,
    .data =
      {
        0xd0, 0x0a, 0x11, 0x0b, 0x09, 0x07, 0x2f,
        0x33, 0x47, 0x38, 0x15, 0x16, 0x2c, 0x32
      },
  },
  {
    .cmd  = 0xf0,
    .len  = 1,
    .data =
      {
        0x3c
      },
  },
  {
    .cmd      = 0xf0,
    .len      = 1,
    .delay_ms = 120,
    .data     =
      {
        0x69
      },
  },
  {
    .cmd = ST7796_INVON,
  },
  {
    .cmd = ST7796_DISPON,
  },
};

static struct st7796_dev_s g_lcd =
{
  .dev =
    {
      .getvideoinfo = st7796_getvideoinfo,
      .getplaneinfo = st7796_getplaneinfo,
      .getpower     = st7796_getpower,
      .setpower     = st7796_setpower,
      .getcontrast  = st7796_getcontrast,
      .setcontrast  = st7796_setcontrast,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int st7796_begin(struct st7796_dev_s *priv)
{
  int ret;

  ret = SPI_LOCK(priv->spi, true);
  if (ret < 0)
    {
      return ret;
    }

  SPI_SETMODE(priv->spi, SPIDEV_MODE0);
  SPI_SETBITS(priv->spi, 8);
  SPI_SETFREQUENCY(priv->spi, BOARD_LCD_SPI_FREQUENCY);
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  return OK;
}

static void st7796_end(struct st7796_dev_s *priv)
{
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(priv->spi, false);
}

static void st7796_command_locked(struct st7796_dev_s *priv, uint8_t cmd,
                                  const uint8_t *data, size_t len)
{
  esp_gpiowrite(BOARD_LCD_DC_PIN, false);
  SPI_SNDBLOCK(priv->spi, &cmd, 1);

  if (len > 0)
    {
      esp_gpiowrite(BOARD_LCD_DC_PIN, true);
      SPI_SNDBLOCK(priv->spi, data, len);
    }
}

static int st7796_command(struct st7796_dev_s *priv, uint8_t cmd,
                          const uint8_t *data, size_t len)
{
  int ret;

  ret = st7796_begin(priv);
  if (ret < 0)
    {
      return ret;
    }

  st7796_command_locked(priv, cmd, data, len);
  st7796_end(priv);
  return OK;
}

static void st7796_setarea_locked(struct st7796_dev_s *priv,
                                  uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1)
{
  uint8_t data[4];

  data[0] = x0 >> 8;
  data[1] = x0 & 0xff;
  data[2] = x1 >> 8;
  data[3] = x1 & 0xff;
  st7796_command_locked(priv, ST7796_CASET, data, sizeof(data));

  data[0] = y0 >> 8;
  data[1] = y0 & 0xff;
  data[2] = y1 >> 8;
  data[3] = y1 & 0xff;
  st7796_command_locked(priv, ST7796_RASET, data, sizeof(data));
}

static void st7796_sendrow_locked(struct st7796_dev_s *priv,
                                  const uint8_t *buffer, size_t npixels)
{
  const uint16_t *src = (const uint16_t *)buffer;
  size_t i;

  /* NuttX RGB565 pixels are native-endian, while the panel SPI stream is
   * big-endian.  Convert one scan line at a time so callers do not need a
   * DMA-capable or byte-swapped buffer.
   */

  for (i = 0; i < npixels; i++)
    {
      priv->txbuffer[2 * i]     = src[i] >> 8;
      priv->txbuffer[2 * i + 1] = src[i] & 0xff;
    }

  SPI_SNDBLOCK(priv->spi, priv->txbuffer,
                npixels * ST7796_BYTES_PER_PIXEL);
}

static int st7796_panel_initialize(struct st7796_dev_s *priv)
{
  static const uint8_t madctl = ST7796_MADCTL_BGR | ST7796_MADCTL_MX;
  static const uint8_t colmod = 0x05;
  size_t i;
  int ret;

  ret = st7796_command(priv, ST7796_SLPOUT, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(100);

  ret = st7796_command(priv, ST7796_MADCTL, &madctl, 1);
  if (ret < 0)
    {
      return ret;
    }

  ret = st7796_command(priv, ST7796_COLMOD, &colmod, 1);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < sizeof(g_st7796_init) / sizeof(g_st7796_init[0]); i++)
    {
      ret = st7796_command(priv, g_st7796_init[i].cmd,
                           g_st7796_init[i].data,
                           g_st7796_init[i].len);
      if (ret < 0)
        {
          return ret;
        }

      if (g_st7796_init[i].delay_ms > 0)
        {
          up_mdelay(g_st7796_init[i].delay_ms);
        }
    }

  ret = st7796_command(priv, ST7796_INVON, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = st7796_setpower(&priv->dev, CONFIG_LCD_MAXPOWER);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(20);
  return OK;
}

static int st7796_putrun(struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, const uint8_t *buffer,
                         size_t npixels)
{
  struct st7796_dev_s *priv = (struct st7796_dev_s *)dev;
  int ret;

  if (buffer == NULL || npixels == 0 || row < 0 || col < 0 ||
      row >= BOARD_LCD_HEIGHT || col + npixels > BOARD_LCD_WIDTH)
    {
      return -EINVAL;
    }

  ret = st7796_begin(priv);
  if (ret < 0)
    {
      return ret;
    }

  st7796_setarea_locked(priv, col, row, col + npixels - 1, row);
  st7796_command_locked(priv, ST7796_RAMWR, NULL, 0);
  esp_gpiowrite(BOARD_LCD_DC_PIN, true);
  st7796_sendrow_locked(priv, buffer, npixels);
  st7796_end(priv);
  return OK;
}

static int st7796_putarea(struct lcd_dev_s *dev,
                          fb_coord_t row_start, fb_coord_t row_end,
                          fb_coord_t col_start, fb_coord_t col_end,
                          const uint8_t *buffer, fb_coord_t stride)
{
  struct st7796_dev_s *priv = (struct st7796_dev_s *)dev;
  size_t cols;
  size_t rows;
  size_t row;
  int ret;

  if (buffer == NULL || row_start < 0 || col_start < 0 ||
      row_end < row_start || col_end < col_start ||
      row_end >= BOARD_LCD_HEIGHT || col_end >= BOARD_LCD_WIDTH)
    {
      return -EINVAL;
    }

  cols = col_end - col_start + 1;
  rows = row_end - row_start + 1;
  if (stride < cols * ST7796_BYTES_PER_PIXEL)
    {
      return -EINVAL;
    }

  ret = st7796_begin(priv);
  if (ret < 0)
    {
      return ret;
    }

  st7796_setarea_locked(priv, col_start, row_start, col_end, row_end);
  st7796_command_locked(priv, ST7796_RAMWR, NULL, 0);
  esp_gpiowrite(BOARD_LCD_DC_PIN, true);

  for (row = 0; row < rows; row++)
    {
      st7796_sendrow_locked(priv, buffer + row * stride, cols);
    }

  st7796_end(priv);
  return OK;
}

static int st7796_getvideoinfo(struct lcd_dev_s *dev,
                               struct fb_videoinfo_s *vinfo)
{
  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = BOARD_LCD_WIDTH;
  vinfo->yres    = BOARD_LCD_HEIGHT;
  vinfo->nplanes = 1;
  return OK;
}

static int st7796_getplaneinfo(struct lcd_dev_s *dev, unsigned int planeno,
                               struct lcd_planeinfo_s *pinfo)
{
  struct st7796_dev_s *priv = (struct st7796_dev_s *)dev;

  if (planeno != 0 || pinfo == NULL)
    {
      return -EINVAL;
    }

  memset(pinfo, 0, sizeof(*pinfo));
  pinfo->putrun  = st7796_putrun;
  pinfo->putarea = st7796_putarea;
  pinfo->buffer  = (uint8_t *)priv->runbuffer;
  pinfo->bpp     = ST7796_BPP;
  pinfo->dev     = dev;
  return OK;
}

static int st7796_getpower(struct lcd_dev_s *dev)
{
  struct st7796_dev_s *priv = (struct st7796_dev_s *)dev;
  return priv->power;
}

static int st7796_setpower(struct lcd_dev_s *dev, int power)
{
  struct st7796_dev_s *priv = (struct st7796_dev_s *)dev;
  uint8_t cmd;
  int ret;

  if (power < 0 || power > CONFIG_LCD_MAXPOWER)
    {
      return -EINVAL;
    }

  cmd = power > 0 ? ST7796_DISPON : ST7796_DISPOFF;
  ret = st7796_command(priv, cmd, NULL, 0);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(BOARD_LCD_BACKLIGHT_PIN, power > 0);
  priv->power = power;
  return OK;
}

static int st7796_getcontrast(struct lcd_dev_s *dev)
{
  return CONFIG_LCD_MAXCONTRAST;
}

static int st7796_setcontrast(struct lcd_dev_s *dev,
                              unsigned int contrast)
{
  return contrast <= CONFIG_LCD_MAXCONTRAST ? OK : -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_lcd_initialize(void)
{
  int ret;

  if (g_lcd.spi != NULL)
    {
      return OK;
    }

  ret = esp_configgpio(BOARD_LCD_DC_PIN, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_configgpio(BOARD_LCD_RESET_PIN, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_configgpio(BOARD_LCD_BACKLIGHT_PIN, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(BOARD_LCD_DC_PIN, true);
  esp_gpiowrite(BOARD_LCD_BACKLIGHT_PIN, false);

  g_lcd.spi = esp_spibus_initialize(BOARD_LCD_SPI_PORT);
  if (g_lcd.spi == NULL)
    {
      return -ENODEV;
    }

  /* ST7796 reset is active low. */

  esp_gpiowrite(BOARD_LCD_RESET_PIN, false);
  up_mdelay(10);
  esp_gpiowrite(BOARD_LCD_RESET_PIN, true);
  up_mdelay(120);

  ret = st7796_panel_initialize(&g_lcd);
  if (ret < 0)
    {
      esp_spibus_uninitialize(g_lcd.spi);
      g_lcd.spi = NULL;
    }

  return ret;
}

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  if (devno != 0 || g_lcd.spi == NULL)
    {
      return NULL;
    }

  return &g_lcd.dev;
}

void board_lcd_uninitialize(void)
{
  if (g_lcd.spi != NULL)
    {
      st7796_setpower(&g_lcd.dev, 0);
      esp_spibus_uninitialize(g_lcd.spi);
      g_lcd.spi = NULL;
    }
}
