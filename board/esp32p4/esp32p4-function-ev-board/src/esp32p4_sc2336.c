/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_sc2336.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/video/imgsensor.h>
#include <nuttx/video/v4l2_cap.h>

#include <sys/videoio.h>

#include "esp_mipi_csi.h"

FAR struct i2c_master_s *esp_i2cbus_initialize(int port);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SC2336_I2C_PORT              0
#define SC2336_I2C_ADDRESS           0x30
#define SC2336_I2C_FREQUENCY         400000
#define SC2336_REG_SLEEP_MODE        0x0100
#define SC2336_REG_SOFTWARE_RESET    0x0103
#define SC2336_REG_PRODUCT_ID_HIGH   0x3107
#define SC2336_REG_PRODUCT_ID_LOW    0x3108
#define SC2336_REG_END               0xffff
#define SC2336_PRODUCT_ID_HIGH       0xcb
#define SC2336_PRODUCT_ID_LOW        0x3a
#define SC2336_RESET_DELAY_US        10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sc2336_reg_s
{
  uint16_t reg;
  uint8_t value;
};

struct sc2336_dev_s
{
  struct imgsensor_s sensor;
  FAR struct i2c_master_s *i2c;
  bool initialized;
  bool streaming;
};

#include "sc2336_720p30_raw10.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool sc2336_is_available(FAR struct imgsensor_s *sensor);
static int sc2336_init(FAR struct imgsensor_s *sensor);
static int sc2336_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *sc2336_get_driver_name(
  FAR struct imgsensor_s *sensor);
static int sc2336_validate(FAR struct imgsensor_s *sensor,
                            imgsensor_stream_type_t type,
                            uint8_t nr_datafmts,
                            FAR imgsensor_format_t *datafmts,
                            FAR imgsensor_interval_t *interval);
static int sc2336_start(FAR struct imgsensor_s *sensor,
                         imgsensor_stream_type_t type,
                         uint8_t nr_datafmts,
                         FAR imgsensor_format_t *datafmts,
                         FAR imgsensor_interval_t *interval);
static int sc2336_stop(FAR struct imgsensor_s *sensor,
                        imgsensor_stream_type_t type);
static int sc2336_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgsensor_ops_s g_sc2336_ops =
{
  .is_available          = sc2336_is_available,
  .init                  = sc2336_init,
  .uninit                = sc2336_uninit,
  .get_driver_name       = sc2336_get_driver_name,
  .validate_frame_setting = sc2336_validate,
  .start_capture         = sc2336_start,
  .stop_capture          = sc2336_stop,
  .get_frame_interval    = sc2336_get_frame_interval,
};

static const struct v4l2_fmtdesc g_sc2336_fmtdesc[] =
{
  {
    .index = 0,
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixelformat = V4L2_PIX_FMT_ENTROPY,
    .description = "Experimental packed BGGR RAW10",
  },
};

static const struct v4l2_frmsizeenum g_sc2336_frmsize[] =
{
  {
    .index = 0,
    .pixel_format = V4L2_PIX_FMT_ENTROPY,
    .type = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete =
    {
      .width = ESP_MIPI_CSI_WIDTH,
      .height = ESP_MIPI_CSI_HEIGHT,
    },
  },
};

static const struct v4l2_frmivalenum g_sc2336_frmival[] =
{
  {
    .index = 0,
    .pixel_format = V4L2_PIX_FMT_ENTROPY,
    .width = ESP_MIPI_CSI_WIDTH,
    .height = ESP_MIPI_CSI_HEIGHT,
    .type = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete =
    {
      .numerator = ESP_MIPI_CSI_FRAME_INTERVAL_NUM,
      .denominator = ESP_MIPI_CSI_FRAME_INTERVAL_DEN,
    },
  },
};

static struct sc2336_dev_s g_sc2336 =
{
  .sensor =
  {
    .ops = &g_sc2336_ops,
    .fmtdescs_num = 1,
    .fmtdescs = g_sc2336_fmtdesc,
    .frmsizes_num = 1,
    .frmsizes = g_sc2336_frmsize,
    .frmintervals_num = 1,
    .frmintervals = g_sc2336_frmival,
  },
};

static bool g_camera_registered;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sc2336_write(FAR struct sc2336_dev_s *priv, uint16_t reg,
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
    .frequency = SC2336_I2C_FREQUENCY,
    .addr = SC2336_I2C_ADDRESS,
    .flags = 0,
    .buffer = buffer,
    .length = sizeof(buffer),
  };

  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

static int sc2336_read(FAR struct sc2336_dev_s *priv, uint16_t reg,
                       FAR uint8_t *value)
{
  uint8_t regbuf[2] =
  {
    reg >> 8,
    reg & 0xff
  };

  struct i2c_msg_s msg[2] =
  {
    {
      .frequency = SC2336_I2C_FREQUENCY,
      .addr = SC2336_I2C_ADDRESS,
      .flags = I2C_M_NOSTOP,
      .buffer = regbuf,
      .length = sizeof(regbuf),
    },
    {
      .frequency = SC2336_I2C_FREQUENCY,
      .addr = SC2336_I2C_ADDRESS,
      .flags = I2C_M_READ,
      .buffer = value,
      .length = 1,
    },
  };

  return I2C_TRANSFER(priv->i2c, msg, 2);
}

static int sc2336_probe(FAR struct sc2336_dev_s *priv)
{
  uint8_t high;
  uint8_t low;
  int ret;

  ret = sc2336_read(priv, SC2336_REG_PRODUCT_ID_HIGH, &high);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_read(priv, SC2336_REG_PRODUCT_ID_LOW, &low);
  if (ret < 0)
    {
      return ret;
    }

  if (high != SC2336_PRODUCT_ID_HIGH || low != SC2336_PRODUCT_ID_LOW)
    {
      syslog(LOG_ERR, "ERROR: SC2336 PID mismatch: %02x%02x\n", high, low);
      return -ENODEV;
    }

  return OK;
}

static int sc2336_program_mode(FAR struct sc2336_dev_s *priv)
{
  FAR const struct sc2336_reg_s *entry;
  int ret;

  ret = sc2336_write(priv, SC2336_REG_SOFTWARE_RESET, 0x01);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(SC2336_RESET_DELAY_US);
  for (entry = g_sc2336_720p30_raw10;
       entry->reg != SC2336_REG_END; entry++)
    {
      ret = sc2336_write(priv, entry->reg, entry->value);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: SC2336 register %04x failed: %d\n",
                 entry->reg, ret);
          return ret;
        }
    }

  return sc2336_write(priv, SC2336_REG_SLEEP_MODE, 0x00);
}

static bool sc2336_setting_valid(imgsensor_stream_type_t type,
                                  uint8_t nr_datafmts,
                                  FAR imgsensor_format_t *datafmts,
                                  FAR imgsensor_interval_t *interval)
{
  return type == IMGSENSOR_STREAM_TYPE_VIDEO && nr_datafmts == 1 &&
         datafmts != NULL && interval != NULL &&
         datafmts[IMGSENSOR_FMT_MAIN].width == ESP_MIPI_CSI_WIDTH &&
         datafmts[IMGSENSOR_FMT_MAIN].height == ESP_MIPI_CSI_HEIGHT &&
         datafmts[IMGSENSOR_FMT_MAIN].pixelformat ==
           IMGSENSOR_PIX_FMT_ENTROPY &&
         interval->numerator == ESP_MIPI_CSI_FRAME_INTERVAL_NUM &&
         interval->denominator == ESP_MIPI_CSI_FRAME_INTERVAL_DEN;
}

static bool sc2336_is_available(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;

  return priv->i2c != NULL && sc2336_probe(priv) == OK;
}

static int sc2336_init(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  if (priv->initialized)
    {
      return OK;
    }

  if (priv->i2c == NULL)
    {
      return -ENODEV;
    }

  ret = sc2336_probe(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_program_mode(priv);
  if (ret >= 0)
    {
      priv->initialized = true;
      priv->streaming = false;
    }

  return ret;
}

static int sc2336_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret = OK;

  if (priv->i2c != NULL && priv->initialized)
    {
      ret = sc2336_write(priv, SC2336_REG_SLEEP_MODE, 0x00);
    }

  priv->streaming = false;
  priv->initialized = false;
  return ret;
}

static FAR const char *sc2336_get_driver_name(
  FAR struct imgsensor_s *sensor)
{
  (void)sensor;
  return "SC2336";
}

static int sc2336_validate(FAR struct imgsensor_s *sensor,
                            imgsensor_stream_type_t type,
                            uint8_t nr_datafmts,
                            FAR imgsensor_format_t *datafmts,
                            FAR imgsensor_interval_t *interval)
{
  (void)sensor;
  return sc2336_setting_valid(type, nr_datafmts, datafmts, interval) ? OK :
         -ENOTSUP;
}

static int sc2336_start(FAR struct imgsensor_s *sensor,
                         imgsensor_stream_type_t type,
                         uint8_t nr_datafmts,
                         FAR imgsensor_format_t *datafmts,
                         FAR imgsensor_interval_t *interval)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  if (!sc2336_setting_valid(type, nr_datafmts, datafmts, interval))
    {
      return -ENOTSUP;
    }

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  if (priv->streaming)
    {
      return -EBUSY;
    }

  ret = sc2336_write(priv, SC2336_REG_SLEEP_MODE, 0x01);
  if (ret >= 0)
    {
      priv->streaming = true;
    }

  return ret;
}

static int sc2336_stop(FAR struct imgsensor_s *sensor,
                        imgsensor_stream_type_t type)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  if (type != IMGSENSOR_STREAM_TYPE_VIDEO)
    {
      return -ENOTSUP;
    }

  if (!priv->streaming)
    {
      return OK;
    }

  ret = sc2336_write(priv, SC2336_REG_SLEEP_MODE, 0x00);
  if (ret >= 0)
    {
      priv->streaming = false;
    }

  return ret;
}

static int sc2336_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval)
{
  (void)sensor;

  if (type != IMGSENSOR_STREAM_TYPE_VIDEO || interval == NULL)
    {
      return -EINVAL;
    }

  interval->numerator = ESP_MIPI_CSI_FRAME_INTERVAL_NUM;
  interval->denominator = ESP_MIPI_CSI_FRAME_INTERVAL_DEN;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_sc2336_initialize(void)
{
  FAR struct imgdata_s *data;
  FAR struct imgsensor_s *sensors[1];
  int ret;

  if (g_camera_registered)
    {
      return OK;
    }

  /* The official Function EV Board BSP marks camera XCLK as GPIO_NUM_NC.
   * This port therefore requires the camera module to supply 24 MHz XCLK.
   */

  g_sc2336.i2c = esp_i2cbus_initialize(SC2336_I2C_PORT);
  if (g_sc2336.i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SC2336 I2C0\n");
      return -ENODEV;
    }

  ret = sc2336_probe(&g_sc2336);
  if (ret < 0)
    {
      return ret;
    }

  data = esp_mipi_csi_initialize();
  if (data == NULL)
    {
      return -ENODEV;
    }

  sensors[0] = &g_sc2336.sensor;
  ret = capture_register("/dev/video0", data, sensors, 1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to register SC2336 camera: %d\n", ret);
      return ret;
    }

  g_camera_registered = true;
  syslog(LOG_WARNING,
         "CSI: /dev/video0 is experimental packed BGGR RAW10; "
         "V4L2 ENTROPY is an opaque transport token, not an image format\n");
  return OK;
}
