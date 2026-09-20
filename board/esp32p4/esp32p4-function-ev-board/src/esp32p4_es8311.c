/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_es8311.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
#include <syslog.h>
#define xiaozhi_alog(level, format, ...)                                       \
  syslog(level, "xiaozhi audio: " format, ##__VA_ARGS__)
#define xiaozhi_ainfo(format, ...) xiaozhi_alog(LOG_INFO, format, ##__VA_ARGS__)
#define xiaozhi_awarn(format, ...)                                             \
  xiaozhi_alog(LOG_WARNING, format, ##__VA_ARGS__)
#define xiaozhi_aerr(format, ...) xiaozhi_alog(LOG_ERR, format, ##__VA_ARGS__)
#else
#define xiaozhi_alog(level, format, ...)                                       \
  do {                                                                         \
  } while (0)
#define xiaozhi_ainfo(format, ...)                                             \
  do {                                                                         \
  } while (0)
#define xiaozhi_awarn(format, ...)                                             \
  do {                                                                         \
  } while (0)
#define xiaozhi_aerr(format, ...)                                              \
  do {                                                                         \
  } while (0)
#endif

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>

#include "espressif/esp_i2c.h"
#include "espressif/esp_i2s.h"
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_ENABLE
#include "espressif/esp_gpio.h"
#endif

#include "esp32p4-function-ev-board.h"

#if defined(CONFIG_AUDIO_ES8311) && defined(CONFIG_ESPRESSIF_I2S0) &&          \
    (defined(CONFIG_ESPRESSIF_I2C0) || defined(CONFIG_ESPRESSIF_I2C1))

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct es8311_i2c_proxy_s {
  struct i2c_master_s dev;
  struct i2c_master_s *native;
};

static int board_es8311_i2c_transfer(struct i2c_master_s *dev,
                                     struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int board_es8311_i2c_reset(struct i2c_master_s *dev);
#endif
static int board_es8311_i2c_setup(struct i2c_master_s *dev);
static int board_es8311_i2c_shutdown(struct i2c_master_s *dev);

static const struct i2c_ops_s g_es8311_i2c_ops = {
    .transfer = board_es8311_i2c_transfer,
#ifdef CONFIG_I2C_RESET
    .reset = board_es8311_i2c_reset,
#endif
    .setup = board_es8311_i2c_setup,
    .shutdown = board_es8311_i2c_shutdown,
};

static struct es8311_lower_s g_es8311_lower[2];
static struct es8311_i2c_proxy_s g_es8311_i2c_proxy = {
    .dev.ops = &g_es8311_i2c_ops,
};
static const struct audio_ops_s *g_es8311_native_ops[2];
static struct audio_ops_s g_es8311_compat_ops[2];
static struct audio_lowerhalf_s *g_es8311_devices[2];
static struct i2s_dev_s *g_es8311_i2s;
static mutex_t g_es8311_lock = NXMUTEX_INITIALIZER;
static bool g_es8311_started[2];
static unsigned int g_es8311_start_count;
static bool g_es8311_initialized;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int board_es8311_i2c_transfer(struct i2c_master_s *dev,
                                     struct i2c_msg_s *msgs, int count) {
  struct es8311_i2c_proxy_s *proxy = (struct es8311_i2c_proxy_s *)dev;

  /* The current NuttX ES8311 register-read helper requests 12 bytes into a
   * one-byte stack variable.  Correct only that driver's exact transaction
   * shape before it reaches the controller, avoiding both stack corruption
   * and any behavior change for ordinary transfers.
   */

  if (count == 2 && msgs[0].length == 1 && (msgs[0].flags & I2C_M_READ) == 0 &&
      msgs[1].length == 12 && (msgs[1].flags & I2C_M_READ) != 0 &&
      msgs[0].addr == msgs[1].addr) {
    struct i2c_msg_s fixed[2];

    fixed[0] = msgs[0];
    fixed[1] = msgs[1];
    fixed[1].length = 1;
    return I2C_TRANSFER(proxy->native, fixed, 2);
  }

  return I2C_TRANSFER(proxy->native, msgs, count);
}

#ifdef CONFIG_I2C_RESET
static int board_es8311_i2c_reset(struct i2c_master_s *dev) {
  struct es8311_i2c_proxy_s *proxy = (struct es8311_i2c_proxy_s *)dev;

  return I2C_RESET(proxy->native);
}
#endif

static int board_es8311_i2c_setup(struct i2c_master_s *dev) {
  struct es8311_i2c_proxy_s *proxy = (struct es8311_i2c_proxy_s *)dev;

  if (proxy->native->ops->setup == NULL) {
    return -ENOSYS;
  }

  return I2C_SETUP(proxy->native);
}

static int board_es8311_i2c_shutdown(struct i2c_master_s *dev) {
  struct es8311_i2c_proxy_s *proxy = (struct es8311_i2c_proxy_s *)dev;

  if (proxy->native->ops->shutdown == NULL) {
    return -ENOSYS;
  }

  return I2C_SHUTDOWN(proxy->native);
}

static int board_es8311_index(struct audio_lowerhalf_s *dev) {
  int index;

  for (index = 0; index < 2; index++) {
    if (g_es8311_devices[index] == dev) {
      return index;
    }
  }

  return -ENODEV;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int board_es8311_configure(struct audio_lowerhalf_s *dev, void *session,
                                  const struct audio_caps_s *caps)
#else
static int board_es8311_configure(struct audio_lowerhalf_s *dev,
                                  const struct audio_caps_s *caps)
#endif
{
  int index;
  int ret;

  index = board_es8311_index(dev);
  if (index < 0) {
    return index;
  }

  ret = nxmutex_lock(&g_es8311_lock);
  if (ret < 0) {
    return ret;
  }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = g_es8311_native_ops[index]->configure(dev, session, caps);
#else
  ret = g_es8311_native_ops[index]->configure(dev, caps);
#endif
  nxmutex_unlock(&g_es8311_lock);

  /* The current NuttX ES8311 configure implementation initializes ret to
   * -ERANGE and accidentally preserves that value when its sample-rate
   * programming succeeds.  The upper half consequently remains in
   * AUDIO_STATE_OPEN and rejects AUDIOIOC_START.  Limit the compatibility
   * conversion to the exact PCM format used by this board; genuine
   * validation errors for other formats remain visible.  Remove this shim
   * once the lower-half return handling is fixed upstream.
   */

  if (ret == -ERANGE &&
      (caps->ac_type == AUDIO_TYPE_INPUT ||
       caps->ac_type == AUDIO_TYPE_OUTPUT) &&
      caps->ac_channels == 1 && caps->ac_controls.hw[0] == 16000 &&
      caps->ac_controls.b[2] == 16) {
    audwarn("ES8311: accepting valid 16-kHz PCM configuration\n");
    return OK;
  }

  return ret;
}

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int board_es8311_start(struct audio_lowerhalf_s *dev, void *session)
#else
static int board_es8311_start(struct audio_lowerhalf_s *dev)
#endif
{
  int index = board_es8311_index(dev);
  int ret;

  if (index < 0) {
    return index;
  }

  ret = nxmutex_lock(&g_es8311_lock);
  if (ret < 0) {
    return ret;
  }

  /* The ES8311 lower half drives samples through the raw I2S interface,
   * but does not forward its audio START/STOP operations to that interface.
   * Without START, the ESP I2S RX completion path treats the first buffer as
   * final and the ES8311 worker terminates immediately.  Playback and
   * capture share the same I2S peripheral, so keep it streaming until both
   * endpoints have stopped.
   */

  if (!g_es8311_started[index] && g_es8311_start_count == 0) {
    ret = I2S_IOCTL(g_es8311_i2s, AUDIOIOC_START, 0);
    if (ret < 0) {
      nxmutex_unlock(&g_es8311_lock);
      return ret;
    }
  }

  xiaozhi_ainfo("ES8311 start endpoint=%d active=%u\n", index,
                g_es8311_start_count);

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = g_es8311_native_ops[index]->start(dev, session);
#else
  ret = g_es8311_native_ops[index]->start(dev);
#endif

  if (ret >= 0 && !g_es8311_started[index]) {
    g_es8311_started[index] = true;
    g_es8311_start_count++;
  } else if (ret < 0 && g_es8311_start_count == 0) {
    I2S_IOCTL(g_es8311_i2s, AUDIOIOC_STOP, 0);
  }

  xiaozhi_ainfo("ES8311 start endpoint=%d ret=%d active=%u\n", index, ret,
                g_es8311_start_count);

  nxmutex_unlock(&g_es8311_lock);
  return ret;
}

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int board_es8311_stop(struct audio_lowerhalf_s *dev, void *session)
#else
static int board_es8311_stop(struct audio_lowerhalf_s *dev)
#endif
{
  int index = board_es8311_index(dev);
  int ret;

  if (index < 0) {
    return index;
  }

  ret = nxmutex_lock(&g_es8311_lock);
  if (ret < 0) {
    return ret;
  }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = g_es8311_native_ops[index]->stop(dev, session);
#else
  ret = g_es8311_native_ops[index]->stop(dev);
#endif

  xiaozhi_ainfo("ES8311 stop endpoint=%d native_ret=%d active=%u\n", index, ret,
                g_es8311_start_count);

  if (ret >= 0 && g_es8311_started[index]) {
    g_es8311_started[index] = false;
    g_es8311_start_count--;

    if (g_es8311_start_count == 0) {
      int i2s_ret = I2S_IOCTL(g_es8311_i2s, AUDIOIOC_STOP, 0);

      if (i2s_ret < 0) {
        ret = i2s_ret;
      }
    }
  }

  xiaozhi_ainfo("ES8311 stop endpoint=%d ret=%d active=%u\n", index, ret,
                g_es8311_start_count);

  nxmutex_unlock(&g_es8311_lock);
  return ret;
}
#endif

static void board_es8311_install_compat(struct audio_lowerhalf_s *dev,
                                        int index) {
  g_es8311_devices[index] = dev;
  g_es8311_native_ops[index] = dev->ops;
  memcpy(&g_es8311_compat_ops[index], dev->ops,
         sizeof(g_es8311_compat_ops[index]));
  g_es8311_compat_ops[index].configure = board_es8311_configure;
  g_es8311_compat_ops[index].start = board_es8311_start;
#ifndef CONFIG_AUDIO_EXCLUDE_STOP
  g_es8311_compat_ops[index].stop = board_es8311_stop;
#endif
  dev->ops = &g_es8311_compat_ops[index];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_es8311_initialize(void) {
  struct audio_lowerhalf_s *capture;
  struct audio_lowerhalf_s *playback;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  if (g_es8311_initialized) {
    return OK;
  }

  i2s = esp_i2sbus_initialize(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_I2S_PORT);
  if (i2s == NULL) {
    auderr("ERROR: Failed to initialize ES8311 I2S bus\n");
    return -ENODEV;
  }

  g_es8311_i2s = i2s;

  i2c = esp_i2cbus_initialize(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_I2C_PORT);
  if (i2c == NULL) {
    auderr("ERROR: Failed to initialize ES8311 I2C bus\n");
    return -ENODEV;
  }

  g_es8311_i2c_proxy.native = i2c;

  g_es8311_lower[0].address =
      CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_I2C_ADDRESS;
  g_es8311_lower[0].frequency =
      CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_I2C_FREQUENCY;
  g_es8311_lower[1] = g_es8311_lower[0];

  playback =
      es8311_initialize(&g_es8311_i2c_proxy.dev, i2s, &g_es8311_lower[0]);
  if (playback == NULL) {
    auderr("ERROR: Failed to initialize ES8311 playback\n");
    return -ENODEV;
  }

  board_es8311_install_compat(playback, 0);

  /* XiaoZhi submits raw PCM buffers after configuring AUDIO_FMT_PCM, so the
   * codec lower half is registered directly rather than through the WAV PCM
   * container decoder.
   */

  ret = audio_register("pcm0", playback);
  if (ret < 0) {
    auderr("ERROR: Failed to register ES8311 playback: %d\n", ret);
    return ret;
  }

  capture = es8311_initialize(&g_es8311_i2c_proxy.dev, i2s, &g_es8311_lower[1]);
  if (capture == NULL) {
    auderr("ERROR: Failed to initialize ES8311 capture\n");
    return -ENODEV;
  }

  board_es8311_install_compat(capture, 1);

  ret = audio_register("pcm_in0", capture);
  if (ret < 0) {
    auderr("ERROR: Failed to register ES8311 capture: %d\n", ret);
    return ret;
  }

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_ENABLE
  esp_configgpio(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_PIN, OUTPUT);
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_ACTIVE_HIGH
  esp_gpiowrite(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_PIN, true);
  xiaozhi_ainfo("speaker PA enabled gpio=%d level=1\n",
                CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_PIN);
#else
  esp_gpiowrite(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_PIN, false);
  xiaozhi_ainfo("speaker PA enabled gpio=%d level=0\n",
                CONFIG_ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_PIN);
#endif
#endif

  g_es8311_initialized = true;
  audinfo("ES8311 registered as /dev/audio/pcm0 and /dev/audio/pcm_in0\n");
  return OK;
}

#else

int board_es8311_initialize(void) { return -ENOSYS; }

#endif
