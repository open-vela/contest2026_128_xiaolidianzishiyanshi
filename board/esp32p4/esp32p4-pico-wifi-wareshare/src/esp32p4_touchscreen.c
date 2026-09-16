/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-pico-wifi-wareshare/src/
 * esp32p4_touchscreen.c
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

#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/input/ft5x06.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#ifndef CONFIG_FT5X06_POLLMODE
static int esp32p4_ft5x06_attach(
  const struct ft5x06_config_s *config, xcpt_t isr, void *arg);
static void esp32p4_ft5x06_enable(
  const struct ft5x06_config_s *config, bool enable);
static void esp32p4_ft5x06_clear(
  const struct ft5x06_config_s *config);
#endif
static void esp32p4_ft5x06_wakeup(
  const struct ft5x06_config_s *config);
static void esp32p4_ft5x06_nreset(
  const struct ft5x06_config_s *config, bool state);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct ft5x06_config_s g_ft5x06_config =
{
  .address   = BOARD_TOUCH_I2C_ADDRESS,
  .frequency = BOARD_TOUCH_I2C_FREQUENCY,
#ifndef CONFIG_FT5X06_POLLMODE
  .attach    = esp32p4_ft5x06_attach,
  .enable    = esp32p4_ft5x06_enable,
  .clear     = esp32p4_ft5x06_clear,
#endif
  .wakeup    = esp32p4_ft5x06_wakeup,
  .nreset    = esp32p4_ft5x06_nreset
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifndef CONFIG_FT5X06_POLLMODE
static int esp32p4_ft5x06_attach(
  const struct ft5x06_config_s *config, xcpt_t isr, void *arg)
{
  return esp_gpio_irq(BOARD_TOUCH_INTERRUPT_PIN, isr, arg);
}

static void esp32p4_ft5x06_enable(
  const struct ft5x06_config_s *config, bool enable)
{
  if (enable)
    {
      esp_gpioirqenable(BOARD_TOUCH_INTERRUPT_PIN);
    }
  else
    {
      esp_gpioirqdisable(BOARD_TOUCH_INTERRUPT_PIN);
    }
}

static void esp32p4_ft5x06_clear(
  const struct ft5x06_config_s *config)
{
  /* The ESP32-P4 GPIO interrupt status is cleared by its ISR service. */
}
#endif

static void esp32p4_ft5x06_wakeup(
  const struct ft5x06_config_s *config)
{
  /* The board does not expose a separate WAKE signal. */
}

static void esp32p4_ft5x06_nreset(
  const struct ft5x06_config_s *config, bool state)
{
  esp_gpiowrite(BOARD_TOUCH_RESET_PIN, state);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_touchscreen_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;

  ret = esp_configgpio(BOARD_TOUCH_RESET_PIN, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

#ifndef CONFIG_FT5X06_POLLMODE
  ret = esp_configgpio(BOARD_TOUCH_INTERRUPT_PIN,
                       INPUT | PULLUP | FALLING);
  if (ret < 0)
    {
      return ret;
    }
#endif

  /* The FT6336 reset input is active low. */

  esp32p4_ft5x06_nreset(&g_ft5x06_config, false);
  up_mdelay(10);
  esp32p4_ft5x06_nreset(&g_ft5x06_config, true);
  up_mdelay(120);

  i2c = esp_i2cbus_initialize(BOARD_TOUCH_I2C_PORT);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  ret = ft5x06_register(i2c, &g_ft5x06_config, 0);
  if (ret < 0)
    {
      esp_i2cbus_uninitialize(i2c);
      return ret;
    }

  return OK;
}
