/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_gt911.c
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

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"

#include "esp32p4-function-ev-board.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT911_ADDR_PRIMARY       0x5d
#define GT911_ADDR_SECONDARY     0x14
#define GT911_CONFIG_REG         0x8047
#define GT911_PRODUCT_ID_REG     0x8140
#define GT911_STATUS_REG         0x814e
#define GT911_FIRST_POINT_REG    0x814f
#define GT911_READY              0x80
#define GT911_POINT_COUNT_MASK   0x0f
#define GT911_POINT_SIZE         8
#define GT911_POLL_INTERVAL_MS   10
#define GT911_PROBE_RETRIES      20
#define GT911_PROBE_DELAY_MS     10
#define GT911_SAMPLE_CACHE       8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gt911_dev_s
{
  struct touch_lowerhalf_s lower;
  struct i2c_master_s *i2c;
  struct work_s work;
  uint16_t last_x;
  uint16_t last_y;
  uint8_t address;
  uint8_t last_id;
  bool pressed;
  bool error_reported;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct gt911_dev_s g_gt911 =
{
  .lower =
  {
    .maxpoint = 1,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gt911_read(FAR struct gt911_dev_s *dev, uint16_t reg,
                      FAR uint8_t *buffer, size_t buflen)
{
  uint8_t regbuf[2] =
  {
    reg >> 8,
    reg & 0xff
  };

  struct i2c_msg_s msg[2] =
  {
    {
      .frequency = BOARD_TOUCH_I2C_FREQUENCY,
      .addr = dev->address,
      /* Match the working ESP-IDF panel-I/O transaction: write the 16-bit
       * register address and issue a repeated START before reading.
       */

      .flags = I2C_M_NOSTOP,
      .buffer = regbuf,
      .length = sizeof(regbuf),
    },
    {
      .frequency = BOARD_TOUCH_I2C_FREQUENCY,
      .addr = dev->address,
      .flags = I2C_M_READ,
      .buffer = buffer,
      .length = buflen,
    },
  };

  return I2C_TRANSFER(dev->i2c, msg, 2);
}

static int gt911_write_u8(FAR struct gt911_dev_s *dev, uint16_t reg,
                          uint8_t value)
{
  uint8_t buffer[3] =
  {
    reg >> 8,
    reg & 0xff,
    value
  };

  struct i2c_msg_s msg =
  {
    .frequency = BOARD_TOUCH_I2C_FREQUENCY,
    .addr = dev->address,
    .flags = 0,
    .buffer = buffer,
    .length = sizeof(buffer),
  };

  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

static int gt911_probe(FAR struct gt911_dev_s *dev)
{
  static const uint8_t addresses[] =
  {
    GT911_ADDR_PRIMARY,
    GT911_ADDR_SECONDARY,
  };

  uint8_t product_id[4] =
  {
    0
  };

  uint8_t config_version;
  unsigned int i;
  unsigned int retry;
  int ret;

  for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
      dev->address = addresses[i];
      for (retry = 0; retry < GT911_PROBE_RETRIES; retry++)
        {
          ret = gt911_read(dev, GT911_PRODUCT_ID_REG, product_id, 3);
          if (ret >= 0)
            {
              ret = gt911_read(dev, GT911_CONFIG_REG, &config_version,
                               sizeof(config_version));
              if (ret < 0)
                {
                  up_mdelay(GT911_PROBE_DELAY_MS);
                  continue;
                }

              syslog(LOG_INFO,
                     "GT911: product %.3s, config version %u detected at "
                     "I2C address 0x%02x\n",
                     product_id, config_version, dev->address);
              return OK;
            }

          up_mdelay(GT911_PROBE_DELAY_MS);
        }

      syslog(LOG_WARNING,
             "GT911: product ID read failed at I2C address 0x%02x "
             "after %u attempts: %d\n",
             dev->address, GT911_PROBE_RETRIES, ret);
    }

  return -ENODEV;
}

static void gt911_report(FAR struct gt911_dev_s *dev,
                         FAR const uint8_t *point)
{
  struct touch_sample_s sample =
  {
    0
  };

  uint16_t raw_x;
  uint16_t raw_y;
  uint16_t pressure;

  raw_x = point[1] | ((uint16_t)point[2] << 8);
  raw_y = point[3] | ((uint16_t)point[4] << 8);
  pressure = point[5] | ((uint16_t)point[6] << 8);

  if (raw_x >= BOARD_TOUCH_WIDTH)
    {
      raw_x = BOARD_TOUCH_WIDTH - 1;
    }

  if (raw_y >= BOARD_TOUCH_HEIGHT)
    {
      raw_y = BOARD_TOUCH_HEIGHT - 1;
    }

  /* The official Function EV Board LCD adapter mounts the GT911 sensor in
   * the opposite X/Y direction from the EK79007 framebuffer.
   */

  dev->last_x = BOARD_TOUCH_WIDTH - 1 - raw_x;
  dev->last_y = BOARD_TOUCH_HEIGHT - 1 - raw_y;
  dev->last_id = point[0] & 0x0f;

  sample.npoints = 1;
  sample.point[0].id = dev->last_id;
  sample.point[0].x = dev->last_x;
  sample.point[0].y = dev->last_y;
  sample.point[0].pressure = pressure;
  sample.point[0].flags = TOUCH_ID_VALID | TOUCH_POS_VALID |
                          TOUCH_PRESSURE_VALID |
                          (dev->pressed ? TOUCH_MOVE : TOUCH_DOWN);

  dev->pressed = true;
  touch_event(dev->lower.priv, &sample);
}

static void gt911_release(FAR struct gt911_dev_s *dev)
{
  struct touch_sample_s sample =
  {
    0
  };

  if (!dev->pressed)
    {
      return;
    }

  sample.npoints = 1;
  sample.point[0].id = dev->last_id;
  sample.point[0].x = dev->last_x;
  sample.point[0].y = dev->last_y;
  sample.point[0].flags = TOUCH_UP | TOUCH_ID_VALID | TOUCH_POS_VALID;

  dev->pressed = false;
  touch_event(dev->lower.priv, &sample);
}

static void gt911_worker(FAR void *arg)
{
  FAR struct gt911_dev_s *dev = arg;
  uint8_t status;
  uint8_t point[GT911_POINT_SIZE];
  int ret;

  ret = gt911_read(dev, GT911_STATUS_REG, &status, sizeof(status));
  if (ret < 0)
    {
      if (!dev->error_reported)
        {
          syslog(LOG_ERR, "ERROR: GT911 status read failed: %d\n", ret);
          dev->error_reported = true;
        }

      goto reschedule;
    }

  dev->error_reported = false;
  if ((status & GT911_READY) != 0)
    {
      if ((status & GT911_POINT_COUNT_MASK) != 0)
        {
          ret = gt911_read(dev, GT911_FIRST_POINT_REG, point,
                           sizeof(point));
          if (ret >= 0)
            {
              gt911_report(dev, point);
            }
        }
      else
        {
          gt911_release(dev);
        }

      ret = gt911_write_u8(dev, GT911_STATUS_REG, 0);
      if (ret < 0 && !dev->error_reported)
        {
          syslog(LOG_ERR, "ERROR: GT911 status clear failed: %d\n", ret);
          dev->error_reported = true;
        }
    }

reschedule:
  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev,
                   MSEC2TICK(GT911_POLL_INTERVAL_MS));
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 polling reschedule failed: %d\n", ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_touchscreen_initialize(void)
{
  FAR struct gt911_dev_s *dev = &g_gt911;
  int ret;

  dev->i2c = esp_i2cbus_initialize(BOARD_TOUCH_I2C_PORT);
  if (dev->i2c == NULL)
    {
      return -ENODEV;
    }

  ret = gt911_probe(dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 not found at 0x5d or 0x14\n");
      return ret;
    }

  ret = touch_register(&dev->lower, BOARD_TOUCH_DEVPATH,
                       GT911_SAMPLE_CACHE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 touch_register failed: %d\n", ret);
      return ret;
    }

  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev,
                   MSEC2TICK(GT911_POLL_INTERVAL_MS));
  if (ret < 0)
    {
      touch_unregister(&dev->lower, BOARD_TOUCH_DEVPATH);
      return ret;
    }

  syslog(LOG_INFO, "GT911: %s registered (polled mode)\n",
         BOARD_TOUCH_DEVPATH);
  return OK;
}
