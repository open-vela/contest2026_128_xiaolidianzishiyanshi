/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/
 * esp_hosted_transport_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <nuttx/sdio.h>

#include "esp32p4_sdmmc.h"
#include "espressif/esp_gpio.h"
#include "port_esp_hosted_host_sdio.h"

#define SDIO_FUNC_0                 0
#define SDIO_FUNC_1                 1

#define SDIO_CCCR_REVISION          0x00
#define SDIO_CCCR_IO_ENABLE         0x02
#define SDIO_CCCR_IO_READY          0x03
#define SDIO_CCCR_INT_ENABLE        0x04
#define SDIO_CCCR_IO_ABORT          0x06
#define SDIO_CCCR_BUS_IF            0x07
#define SDIO_CCCR_HIGHSPEED         0x13
#define SDIO_FBR1_INTERFACE_CODE    0x100

#define SDIO_CCCR_IO_ABORT_RESET    (1u << 3)
#define SDIO_CCCR_HIGHSPEED_SUPPORT (1u << 0)
#define SDIO_CCCR_HIGHSPEED_ENABLE  (1u << 1)
#define SDIO_CMD52_WRITE            (1u << 31)
#define SDIO_CMD52_ADDRESS_SHIFT    9

#define SDIO_CMD8_ARGUMENT          0x000001aau

#define ESP_HOSTED_RESET_ASSERT_MS  20
#define ESP_HOSTED_MIN_RESET_BOOT_MS 1500

#if defined(CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS) && \
    CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS > ESP_HOSTED_MIN_RESET_BOOT_MS
#  define ESP_HOSTED_RESET_BOOT_MS CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS
#else
#  define ESP_HOSTED_RESET_BOOT_MS ESP_HOSTED_MIN_RESET_BOOT_MS
#endif

#define ESP_HOSTED_SDIO_IDLE_MS     20
#define ESP_HOSTED_CMD5_RETRIES     100
#define ESP_HOSTED_CMD5_DELAY_MS    10

/* Keep each CMD53 within one ESP32-P4 SDMMC IDMAC descriptor.  Streaming
 * mode can report tens of KiB queued at once; issuing that as one CMD53
 * requires a descriptor chain and can leave the IDMAC stopped at an RX
 * FIFO watermark.  Multiple consecutive CMD53 operations preserve the
 * incrementing-address semantics while avoiding that failure mode.
 */

#define ESP_HOSTED_SDIO_DMA_DESC_SIZE       4096
#define ESP_HOSTED_SDIO_BLOCKS_PER_TRANSFER \
  (ESP_HOSTED_SDIO_DMA_DESC_SIZE / ESP_HOSTED_SDIO_BLOCK_SIZE)

#define ESP_HOSTED_SDIO_OCR_READY   (1u << 31)
#define ESP_HOSTED_SDIO_OCR_NFUNCS_SHIFT 28
#define ESP_HOSTED_SDIO_OCR_NFUNCS_MASK  7u
#define ESP_HOSTED_SDIO_OCR_VOLTAGE_MASK 0x00ff8000u
#define ESP_HOSTED_SDIO_REG_MASK     0x3ffu

struct hosted_sdio_context_s
{
  struct sdio_dev_s *dev;
  mutex_t lock;
  bool attached;
  bool probed;
};

static struct hosted_sdio_context_s g_sdio;

static int hosted_sdio_delay_ms(unsigned int milliseconds)
{
  struct timespec request;
  struct timespec remaining;
  int ret;

  /* up_mdelay() is a CPU loop calibrated by CONFIG_BOARD_LOOPSPERMSEC.
   * That calibration is not accurate on the ESP32-P4 and made a requested
   * 1500 ms delay last only about 500 ms.  Use the scheduler clock here and
   * resume the remaining interval if a signal interrupts the sleep.
   */

  request.tv_sec = milliseconds / MSEC_PER_SEC;
  request.tv_nsec = (milliseconds % MSEC_PER_SEC) * NSEC_PER_MSEC;

  do
    {
      ret = nxsig_nanosleep(&request, &remaining);
      if (ret == -EINTR)
        {
          request = remaining;
        }
    }
  while (ret == -EINTR);

  return ret;
}

static int hosted_sdio_send_command(struct sdio_dev_s *dev,
                                    const char *stage, uint32_t command,
                                    uint32_t argument)
{
  int ret;

  ret = SDIO_SENDCMD(dev, command, argument);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: %s command submission failed: %d "
             "(arg=%08lx)\n",
             stage, ret, (unsigned long)argument);
      return ret;
    }

  ret = SDIO_WAITRESPONSE(dev, command);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: %s response wait failed: %d (arg=%08lx)\n",
             stage, ret, (unsigned long)argument);
    }

  return ret;
}

static int hosted_sdio_reset_io(struct sdio_dev_s *dev)
{
  uint32_t argument;
  uint32_t response;
  int ret;

  /* Follow the SDIO re-initialization sequence used by ESP-IDF.  Function
   * zero's CCCR I/O Abort register has a RES bit which resets all I/O
   * functions.  A card is allowed to stop responding while carrying out
   * this reset, so a response timeout or CRC/response error is expected and
   * must not abort enumeration.
   */

  argument = SDIO_CMD52_WRITE |
             (SDIO_CCCR_IO_ABORT << SDIO_CMD52_ADDRESS_SHIFT) |
             SDIO_CCCR_IO_ABORT_RESET;

  syslog(LOG_INFO,
         "ESP-Hosted: probe stage CMD52 I/O reset (arg=%08lx)\n",
         (unsigned long)argument);

  ret = SDIO_SENDCMD(dev, SDIO_CMD52, argument);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD52 I/O reset submission failed: %d\n", ret);
      return ret;
    }

  ret = SDIO_WAITRESPONSE(dev, SDIO_CMD52);
  if (ret == -ETIMEDOUT || ret == -EIO)
    {
      syslog(LOG_INFO,
             "ESP-Hosted: CMD52 I/O reset completed without a valid R5\n");
      return OK;
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD52 I/O reset response wait failed: %d\n",
             ret);
      return ret;
    }

  ret = SDIO_RECVR5(dev, SDIO_CMD52, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD52 I/O reset R5 read failed: %d\n", ret);
      return ret;
    }

  return OK;
}

static int hosted_sdio_send_if_cond(struct sdio_dev_s *dev)
{
  uint32_t response;
  int ret;

  /* CMD8 is a memory-card capability probe.  An SDIO-only device normally
   * does not answer it, which is not an enumeration failure.  Sending it is
   * nevertheless part of the SD/SDIO reset sequence and is required before
   * CMD5 by the ESP32-C6 hosted firmware.
   */

  syslog(LOG_INFO,
         "ESP-Hosted: probe stage CMD8 interface condition "
         "(arg=%08lx)\n", (unsigned long)SDIO_CMD8_ARGUMENT);

  ret = SDIO_SENDCMD(dev, SD_CMD8, SDIO_CMD8_ARGUMENT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD8 submission failed: %d\n", ret);
      return ret;
    }

  ret = SDIO_WAITRESPONSE(dev, SD_CMD8);
  if (ret == -ETIMEDOUT)
    {
      syslog(LOG_INFO,
             "ESP-Hosted: CMD8 timed out as expected for an SDIO-only "
             "device\n");
      return OK;
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD8 response wait failed: %d\n", ret);
      return ret;
    }

  ret = SDIO_RECVR7(dev, SD_CMD8, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD8 R7 read failed: %d\n", ret);
      return ret;
    }

  if ((response & 0xfffu) != SDIO_CMD8_ARGUMENT)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD8 R7 mismatch: %08lx\n",
             (unsigned long)response);
      return -EIO;
    }

  return OK;
}

static int hosted_sdio_probe_card(struct sdio_dev_s *dev)
{
  uint32_t response;
  uint32_t ocr;
  unsigned int functions;
  int retry;
  int ret;

  nxmutex_init(&dev->mutex);

#ifdef CONFIG_SDIO_MUXBUS
  ret = SDIO_LOCK(dev, true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: SDMMC bus lock failed: %d\n", ret);
      return ret;
    }
#endif

  ret = hosted_sdio_reset_io(dev);
  if (ret < 0)
    {
      goto out;
    }

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD0 GO_IDLE\n");
  ret = hosted_sdio_send_command(dev, "CMD0", MMCSD_CMD0, 0);
  if (ret < 0)
    {
      goto out;
    }

  ret = hosted_sdio_delay_ms(ESP_HOSTED_SDIO_IDLE_MS);
  if (ret < 0)
    {
      goto out;
    }

  ret = hosted_sdio_send_if_cond(dev);
  if (ret < 0)
    {
      goto out;
    }

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD5 OCR inquiry\n");
  ret = hosted_sdio_send_command(dev, "CMD5 inquiry", SDIO_CMD5, 0);
  if (ret < 0)
    {
      goto out;
    }

  ret = SDIO_RECVR4(dev, SDIO_CMD5, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD5 inquiry R4 read failed: %d\n",
             ret);
      goto out;
    }

  functions = (response >> ESP_HOSTED_SDIO_OCR_NFUNCS_SHIFT) &
              ESP_HOSTED_SDIO_OCR_NFUNCS_MASK;
  ocr = response & ESP_HOSTED_SDIO_OCR_VOLTAGE_MASK;
  syslog(LOG_INFO,
         "ESP-Hosted: CMD5 inquiry R4=%08lx functions=%u ocr=%08lx\n",
         (unsigned long)response, functions, (unsigned long)ocr);

  if (functions == 0 || ocr == 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD5 response is not a usable SDIO device\n");
      ret = -ENODEV;
      goto out;
    }

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD5 wait for I/O ready\n");
  for (retry = 0; retry < ESP_HOSTED_CMD5_RETRIES; retry++)
    {
      ret = hosted_sdio_send_command(dev, "CMD5 ready poll", SDIO_CMD5,
                                     ocr);
      if (ret < 0)
        {
          goto out;
        }

      ret = SDIO_RECVR4(dev, SDIO_CMD5, &response);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ESP-Hosted: CMD5 ready R4 read failed: %d\n",
                 ret);
          goto out;
        }

      if ((response & ESP_HOSTED_SDIO_OCR_READY) != 0)
        {
          break;
        }

      ret = hosted_sdio_delay_ms(ESP_HOSTED_CMD5_DELAY_MS);
      if (ret < 0)
        {
          goto out;
        }
    }

  if (retry == ESP_HOSTED_CMD5_RETRIES)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD5 did not become ready, last R4=%08lx\n",
             (unsigned long)response);
      ret = -ETIMEDOUT;
      goto out;
    }

  syslog(LOG_INFO,
         "ESP-Hosted: CMD5 ready after %d poll(s), R4=%08lx\n",
         retry + 1, (unsigned long)response);

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD3 assign RCA\n");
  ret = hosted_sdio_send_command(dev, "CMD3", SD_CMD3, 0);
  if (ret < 0)
    {
      goto out;
    }

  ret = SDIO_RECVR6(dev, SD_CMD3, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD3 R6 read failed: %d\n", ret);
      goto out;
    }

  ocr = response & 0xffff0000u;
  syslog(LOG_INFO, "ESP-Hosted: CMD3 R6=%08lx RCA=%04lx\n",
         (unsigned long)response, (unsigned long)(ocr >> 16));

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD7 select card\n");
  ret = hosted_sdio_send_command(dev, "CMD7", MMCSD_CMD7S, ocr);
  if (ret < 0)
    {
      goto out;
    }

  ret = SDIO_RECVR1(dev, MMCSD_CMD7S, &response);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CMD7 R1 read failed: %d\n", ret);
      goto out;
    }

  syslog(LOG_INFO, "ESP-Hosted: CMD7 R1=%08lx\n",
         (unsigned long)response);

out:
#ifdef CONFIG_SDIO_MUXBUS
  SDIO_LOCK(dev, false);
#endif

  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "ESP-Hosted: probe stage CMD52 select 4-bit bus\n");
  ret = sdio_set_wide_bus(dev);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: CMD52 4-bit bus configuration failed: %d\n",
             ret);
    }

  return ret;
}

static int hosted_sdio_reset_slave(void)
{
  int ret;

  ret = esp_configgpio(CONFIG_ESP_HOSTED_GPIO_RESET_SLAVE, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: reset GPIO %d configuration failed: %d\n",
             CONFIG_ESP_HOSTED_GPIO_RESET_SLAVE, ret);
      return ret;
    }

  esp_gpiowrite(CONFIG_ESP_HOSTED_GPIO_RESET_SLAVE, false);
  ret = hosted_sdio_delay_ms(ESP_HOSTED_RESET_ASSERT_MS);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: reset assertion delay failed: %d\n", ret);
      return ret;
    }

  esp_gpiowrite(CONFIG_ESP_HOSTED_GPIO_RESET_SLAVE, true);
  syslog(LOG_INFO,
         "ESP-Hosted: slave reset released; waiting %u ms before CMD52\n",
         ESP_HOSTED_RESET_BOOT_MS);

  ret = hosted_sdio_delay_ms(ESP_HOSTED_RESET_BOOT_MS);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: slave boot delay failed: %d\n", ret);
    }

  return ret;
}

static int hosted_sdio_readb(struct sdio_dev_s *dev, uint8_t function,
                             uint32_t address, uint8_t *value)
{
  return sdio_io_rw_direct(dev, false, function, address, 0, value);
}

static int hosted_sdio_writeb(struct sdio_dev_s *dev, uint8_t function,
                              uint32_t address, uint8_t value)
{
  return sdio_io_rw_direct(dev, true, function, address, value, NULL);
}

static int hosted_sdio_enable_highspeed(struct sdio_dev_s *dev)
{
  uint8_t speed;
  int ret;

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_HIGHSPEED, &speed);
  if (ret < 0)
    {
      return ret;
    }

  if ((speed & SDIO_CCCR_HIGHSPEED_SUPPORT) == 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: SDIO slave does not support high-speed mode\n");
      return -ENOTSUP;
    }

  speed |= SDIO_CCCR_HIGHSPEED_ENABLE;
  ret = hosted_sdio_writeb(dev, SDIO_FUNC_0, SDIO_CCCR_HIGHSPEED, speed);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_HIGHSPEED, &speed);
  if (ret < 0)
    {
      return ret;
    }

  if ((speed & (SDIO_CCCR_HIGHSPEED_SUPPORT |
                SDIO_CCCR_HIGHSPEED_ENABLE)) !=
               (SDIO_CCCR_HIGHSPEED_SUPPORT |
                SDIO_CCCR_HIGHSPEED_ENABLE))
    {
      syslog(LOG_ERR,
             "ESP-Hosted: SDIO high-speed mode did not latch: %02x\n",
             speed);
      return -EIO;
    }

  return OK;
}

static int hosted_sdio_dump_identity(struct sdio_dev_s *dev)
{
  uint8_t revision;
  uint8_t io_enable;
  uint8_t io_ready;
  uint8_t int_enable;
  uint8_t bus_if;
  uint8_t interface_code;
  int ret;

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_REVISION, &revision);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_IO_ENABLE,
                          &io_enable);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_IO_READY, &io_ready);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_INT_ENABLE,
                          &int_enable);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_CCCR_BUS_IF, &bus_if);
  if (ret < 0)
    {
      return ret;
    }

  ret = hosted_sdio_readb(dev, SDIO_FUNC_0, SDIO_FBR1_INTERFACE_CODE,
                          &interface_code);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "ESP-Hosted: CCCR rev=%u SDIO rev=%u IOE=%02x IOR=%02x "
         "IEN=%02x BUS_IF=%02x F1_IF=%02x\n",
         revision & 0x0f, revision >> 4, io_enable, io_ready,
         int_enable, bus_if, interface_code);
  return OK;
}

int hosted_sdio_card_init(void *ctx, bool show_config)
{
  struct hosted_sdio_context_s *priv = ctx;
  int ret;

  (void)show_config;

  if (priv == NULL || priv->dev == NULL)
    {
      return -EINVAL;
    }

  if (priv->probed)
    {
      return OK;
    }

  if (!priv->attached)
    {
      ret = SDIO_ATTACH(priv->dev);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ESP-Hosted: SDMMC IRQ attach failed: %d\n", ret);
          return ret;
        }

      priv->attached = true;
    }

  SDIO_CLOCK(priv->dev, CLOCK_IDMODE);

  ret = hosted_sdio_probe_card(priv->dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: SDIO probe failed: %d\n", ret);
      return ret;
    }

  ret = sdio_enable_function(priv->dev, SDIO_FUNC_1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: enabling SDIO function 1 failed: %d\n",
             ret);
      return ret;
    }

  ret = sdio_set_blocksize(priv->dev, SDIO_FUNC_0,
                           ESP_HOSTED_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: setting function 0 block size failed: %d\n",
             ret);
      return ret;
    }

  ret = sdio_set_blocksize(priv->dev, SDIO_FUNC_1,
                           ESP_HOSTED_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: setting function 1 block size failed: %d\n",
             ret);
      return ret;
    }

  /* CCCR IEN bit 0 is the interrupt master enable; bit 1 enables the
   * ESP-Hosted data function.
   */

  ret = sdio_enable_interrupt(priv->dev, SDIO_FUNC_0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: enabling SDIO interrupt master failed: %d\n",
             ret);
      return ret;
    }

  ret = sdio_enable_interrupt(priv->dev, SDIO_FUNC_1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: enabling function 1 interrupt failed: %d\n",
             ret);
      return ret;
    }

  ret = hosted_sdio_dump_identity(priv->dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ESP-Hosted: CCCR/FBR read failed: %d\n", ret);
      return ret;
    }

#if CONFIG_ESP_HOSTED_SDIO_FREQ_KHZ >= 40000
  ret = hosted_sdio_enable_highspeed(priv->dev);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: enabling SDIO high-speed mode failed: %d\n",
             ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH_4BIT
  SDIO_CLOCK(priv->dev, CLOCK_SD_TRANSFER_4BIT);
#else
  SDIO_CLOCK(priv->dev, CLOCK_SD_TRANSFER_1BIT);
#endif

  syslog(LOG_INFO, "ESP-Hosted: SDIO transfer clock set to %u kHz, "
                   "%u-bit bus\n",
         CONFIG_ESP_HOSTED_SDIO_FREQ_KHZ,
#ifdef CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH_4BIT
         4u
#else
         1u
#endif
         );

  priv->probed = true;
  syslog(LOG_INFO,
         "ESP-Hosted: ESP32-C6 SDIO function detected on SDMMC slot %d\n",
         CONFIG_ESP_HOSTED_SDIO_SLOT);
  return OK;
}

void *hosted_sdio_init(void)
{
  struct sdio_dev_s *dev;

  if (g_sdio.dev != NULL)
    {
      return &g_sdio;
    }

  /* Configure the host and put pull-ups on the SDIO pins before releasing
   * the ESP32-C6 from reset.  Apart from keeping the bus at valid idle
   * levels while the slave boots, this intentionally places the 1.5 second
   * C6 boot interval between SDMMC initialization and the first command.
   */

  dev = sdio_initialize(CONFIG_ESP_HOSTED_SDIO_SLOT);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "ESP-Hosted: SDMMC slot %d initialization failed\n",
             CONFIG_ESP_HOSTED_SDIO_SLOT);
      return NULL;
    }

  g_sdio.dev = dev;
  nxmutex_init(&g_sdio.lock);
  return &g_sdio;
}

int hosted_sdio_deinit(void *ctx)
{
  struct hosted_sdio_context_s *priv = ctx;

  if (priv == NULL)
    {
      return -EINVAL;
    }

  nxmutex_destroy(&priv->lock);
  priv->dev = NULL;
  priv->attached = false;
  priv->probed = false;
  return OK;
}

int hosted_sdio_card_deinit(void *ctx)
{
  struct hosted_sdio_context_s *priv = ctx;

  if (priv == NULL)
    {
      return -EINVAL;
    }

  priv->probed = false;
  return OK;
}

int esp_hosted_sdio_probe(void)
{
  void *ctx = hosted_sdio_init();
  int ret;

  if (ctx == NULL)
    {
      return -ENODEV;
    }

  ret = hosted_sdio_reset_slave();
  if (ret < 0)
    {
      return ret;
    }

  return hosted_sdio_card_init(ctx, true);
}

int hosted_sdio_read_reg(void *ctx, uint32_t reg, uint8_t *data,
                         uint16_t size, bool lock_required)
{
  struct hosted_sdio_context_s *priv = ctx;
  uint16_t i;
  int ret = OK;

  if (priv == NULL || priv->dev == NULL || data == NULL)
    {
      return -EINVAL;
    }

  if (lock_required)
    {
      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* ESP-Hosted exposes the SLC registers through a 1 KiB SDIO function-1
   * window.  The transport core passes the native ESP peripheral addresses,
   * so translate them before issuing CMD52.
   */

  reg &= ESP_HOSTED_SDIO_REG_MASK;
  for (i = 0; i < size; i++)
    {
      ret = hosted_sdio_readb(priv->dev, SDIO_FUNC_1, reg + i, &data[i]);
      if (ret < 0)
        {
          break;
        }
    }

  if (lock_required)
    {
      nxmutex_unlock(&priv->lock);
    }

  return ret;
}

int hosted_sdio_write_reg(void *ctx, uint32_t reg, uint8_t *data,
                          uint16_t size, bool lock_required)
{
  struct hosted_sdio_context_s *priv = ctx;
  uint16_t i;
  int ret = OK;

  if (priv == NULL || priv->dev == NULL || data == NULL)
    {
      return -EINVAL;
    }

  if (lock_required)
    {
      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          return ret;
        }
    }

  reg &= ESP_HOSTED_SDIO_REG_MASK;
  for (i = 0; i < size; i++)
    {
      ret = hosted_sdio_writeb(priv->dev, SDIO_FUNC_1, reg + i, data[i]);
      if (ret < 0)
        {
          break;
        }
    }

  if (lock_required)
    {
      nxmutex_unlock(&priv->lock);
    }

  return ret;
}

static int hosted_sdio_transfer(void *ctx, bool write, uint32_t reg,
                                uint8_t *data, uint16_t size,
                                bool lock_required)
{
  struct hosted_sdio_context_s *priv = ctx;
  unsigned int transfer_blocks;
  unsigned int transfer_size;
  unsigned int blocks;
  unsigned int remainder;
  uint32_t address;
  uint8_t *buffer;
  int ret = OK;

  if (priv == NULL || priv->dev == NULL || data == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (lock_required)
    {
      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          return ret;
        }
    }

  blocks = size / ESP_HOSTED_SDIO_BLOCK_SIZE;
  remainder = size % ESP_HOSTED_SDIO_BLOCK_SIZE;
  address = reg;
  buffer = data;

  while (blocks != 0)
    {
      transfer_blocks = blocks > ESP_HOSTED_SDIO_BLOCKS_PER_TRANSFER ?
                        ESP_HOSTED_SDIO_BLOCKS_PER_TRANSFER : blocks;
      transfer_size = transfer_blocks * ESP_HOSTED_SDIO_BLOCK_SIZE;
      ret = sdio_io_rw_extended(priv->dev, write, SDIO_FUNC_1, address,
                                true, buffer, ESP_HOSTED_SDIO_BLOCK_SIZE,
                                transfer_blocks);
      if (ret < 0)
        {
          break;
        }

      blocks -= transfer_blocks;
      address += transfer_size;
      buffer += transfer_size;
    }

  if (ret >= 0 && remainder != 0)
    {
      ret = sdio_io_rw_extended(priv->dev, write, SDIO_FUNC_1, address,
                                true, buffer, remainder, 0);
    }

  if (lock_required)
    {
      nxmutex_unlock(&priv->lock);
    }

  return ret;
}

int hosted_sdio_read_block(void *ctx, uint32_t reg, uint8_t *data,
                           uint16_t size, bool lock_required)
{
  return hosted_sdio_transfer(ctx, false, reg, data, size, lock_required);
}

int hosted_sdio_write_block(void *ctx, uint32_t reg, uint8_t *data,
                            uint16_t size, bool lock_required)
{
  return hosted_sdio_transfer(ctx, true, reg, data, size, lock_required);
}

int hosted_sdio_wait_slave_intr(void *ctx, uint32_t ticks_to_wait)
{
  struct hosted_sdio_context_s *priv = ctx;
  int ret;

  if (priv == NULL || priv->dev == NULL)
    {
      return -EINVAL;
    }

  /* Match ESP-IDF's sdmmc_io_wait_int(): the normal RX path passes
   * HOSTED_BLOCK_MAX and sleeps until the slave asserts DAT1.  Do not use a
   * periodic timeout as a reason to poll SDIO registers.
   */

  ret = esp32p4_sdio_wait_card_interrupt(priv->dev, ticks_to_wait);

  return ret;
}
