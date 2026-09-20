/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_mipi_csi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP_MIPI_CSI_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP_MIPI_CSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/imgdata.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The current NuttX imgdata ABI has no Bayer RAW10 value.  ENTROPY is used
 * only as an opaque transport token.  The payload remains packed BGGR RAW10.
 */

#define ESP_MIPI_CSI_PIX_FMT_RAW10       IMGDATA_PIX_FMT_ENTROPY
#define ESP_MIPI_CSI_WIDTH               1280
#define ESP_MIPI_CSI_HEIGHT              720
#define ESP_MIPI_CSI_BITS_PER_PIXEL      10
#define ESP_MIPI_CSI_LANES               2
#define ESP_MIPI_CSI_LANE_RATE_MBPS      405
#define ESP_MIPI_CSI_FRAME_INTERVAL_NUM  1
#define ESP_MIPI_CSI_FRAME_INTERVAL_DEN  30
#define ESP_MIPI_CSI_FRAME_SIZE          \
  (ESP_MIPI_CSI_WIDTH * ESP_MIPI_CSI_HEIGHT * \
   ESP_MIPI_CSI_BITS_PER_PIXEL / 8)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

FAR struct imgdata_s *esp_mipi_csi_initialize(void);

/* MIPI DSI and CSI use different DW-GDMA channels but share one peripheral
 * interrupt source.  When DSI is enabled its ISR dispatches CSI channel
 * completion through this entry point.
 */

int esp_mipi_csi_dma_interrupt(int irq, FAR void *context, FAR void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP_MIPI_CSI_H */
