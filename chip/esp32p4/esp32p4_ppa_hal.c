/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa_hal.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPA operation HAL boundary.  The operation-specific register programming
 * is being kept behind this interface so the public driver remains
 * independent from DMA2D channel and interrupt management.
 ****************************************************************************/

#include <nuttx/config.h>

#include "esp32p4_ppa_internal.h"
#include "hal/ppa_ll.h"

static ppa_fill_color_mode_t
esp32p4_ppa_hal_fill_mode(enum esp32p4_ppa_color_mode_e mode)
{
  switch (mode)
    {
      case ESP32P4_PPA_COLOR_MODE_RGB565:
        return PPA_FILL_COLOR_MODE_RGB565;
      case ESP32P4_PPA_COLOR_MODE_RGB888:
        return PPA_FILL_COLOR_MODE_RGB888;
      default:
        return PPA_FILL_COLOR_MODE_ARGB8888;
    }
}

static ppa_blend_color_mode_t
esp32p4_ppa_hal_blend_mode(enum esp32p4_ppa_color_mode_e mode)
{
  switch (mode)
    {
      case ESP32P4_PPA_COLOR_MODE_RGB565:
        return PPA_BLEND_COLOR_MODE_RGB565;
      case ESP32P4_PPA_COLOR_MODE_RGB888:
        return PPA_BLEND_COLOR_MODE_RGB888;
      case ESP32P4_PPA_COLOR_MODE_A8:
        return PPA_BLEND_COLOR_MODE_A8;
      default:
        return PPA_BLEND_COLOR_MODE_ARGB8888;
    }
}

static ppa_srm_color_mode_t
esp32p4_ppa_hal_srm_mode(enum esp32p4_ppa_color_mode_e mode)
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

static uint32_t
esp32p4_ppa_hal_fill_color(enum esp32p4_ppa_color_mode_e mode,
                           uint32_t color)
{
  uint32_t red;
  uint32_t green;
  uint32_t blue;

  if (mode != ESP32P4_PPA_COLOR_MODE_RGB565)
    {
      return color;
    }

  red = (color >> 11) & 0x1f;
  green = (color >> 5) & 0x3f;
  blue = color & 0x1f;

  red = (red << 3) | (red >> 2);
  green = (green << 2) | (green >> 4);
  blue = (blue << 3) | (blue >> 2);
  return (red << 16) | (green << 8) | blue;
}

void esp32p4_ppa_hal_configure_fill(
  ppa_dev_t *dev, const struct esp32p4_ppa_fill_config_s *config,
  uint32_t *configured_color)
{
  ppa_fill_color_mode_t mode;
  uint32_t color;

  mode = esp32p4_ppa_hal_fill_mode(config->output.color_mode);
  color = esp32p4_ppa_hal_fill_color(config->output.color_mode,
                                     config->color);
  ppa_ll_blend_reset(dev);
  ppa_ll_blend_configure_filling_block(dev, mode, &color,
                                       config->output.block_width,
                                       config->output.block_height);
  ppa_ll_blend_set_tx_color_mode(dev, mode);

  if (configured_color != NULL)
    {
      *configured_color = dev->blend_fix_pixel.val;
    }
}

void esp32p4_ppa_hal_configure_blend(
  ppa_dev_t *dev, const struct esp32p4_ppa_blend_config_s *config)
{
  color_pixel_rgb888_data_t fixed_rgb;
  color_pixel_rgb888_data_t disabled_low = {.val = 0xffffff};
  color_pixel_rgb888_data_t disabled_high = {.val = 0x000000};
  color_pixel_rgb888_data_t bg_low;
  color_pixel_rgb888_data_t bg_high;
  color_pixel_rgb888_data_t fg_low;
  color_pixel_rgb888_data_t fg_high;
  color_pixel_rgb888_data_t ck_default;

  ppa_ll_blend_reset(dev);
  ppa_ll_blend_set_rx_bg_color_mode(dev,
    esp32p4_ppa_hal_blend_mode(config->background.color_mode));
  ppa_ll_blend_enable_rx_bg_byte_swap(dev,
    config->background_byte_swap != 0);
  ppa_ll_blend_enable_rx_bg_rgb_swap(dev,
    config->background_rgb_swap != 0);
  ppa_ll_blend_configure_rx_bg_alpha(dev,
    (ppa_alpha_update_mode_t)config->background_alpha_mode,
    config->background_alpha);
  ppa_ll_blend_set_rx_fg_color_mode(dev,
    esp32p4_ppa_hal_blend_mode(config->foreground.color_mode));
  fixed_rgb.val = config->foreground_color & 0x00ffffff;
  if (config->foreground.color_mode == ESP32P4_PPA_COLOR_MODE_A8)
    {
      ppa_ll_blend_set_rx_fg_fix_rgb(dev, &fixed_rgb);
    }

  ppa_ll_blend_enable_rx_fg_byte_swap(dev,
    config->foreground_byte_swap != 0);
  ppa_ll_blend_enable_rx_fg_rgb_swap(dev,
    config->foreground_rgb_swap != 0);
  ppa_ll_blend_configure_rx_fg_alpha(dev,
    (ppa_alpha_update_mode_t)config->foreground_alpha_mode,
    config->foreground_alpha);
  ppa_ll_blend_set_tx_color_mode(dev,
    esp32p4_ppa_hal_blend_mode(config->output.color_mode));
  bg_low.val = config->background_color_key_low;
  bg_high.val = config->background_color_key_high;
  fg_low.val = config->foreground_color_key_low;
  fg_high.val = config->foreground_color_key_high;
  ck_default.val = config->color_key_default;
  ppa_ll_blend_configure_rx_bg_ck_range(dev,
    config->background_color_key_enable ? &bg_low : &disabled_low,
    config->background_color_key_enable ? &bg_high : &disabled_high);
  ppa_ll_blend_configure_rx_fg_ck_range(dev,
    config->foreground_color_key_enable ? &fg_low : &disabled_low,
    config->foreground_color_key_enable ? &fg_high : &disabled_high);
  ppa_ll_blend_set_ck_default_rgb(dev, &ck_default);
  ppa_ll_blend_enable_ck_fg_bg_reverse(dev,
    config->color_key_reverse != 0);
}

void esp32p4_ppa_hal_configure_srm(
  ppa_dev_t *dev, const struct esp32p4_ppa_srm_config_s *config)
{
  ppa_srm_color_mode_t output_mode;
  uint32_t scale_x_int;
  uint32_t scale_x_frag;
  uint32_t scale_y_int;
  uint32_t scale_y_frag;
  uint32_t width_out;
  uint32_t width_divisor;
  uint32_t width_left;
  uint32_t height_in_left;
  uint32_t height_left;
  uint32_t output_depth;
  bool bypass_mb_order;

  scale_x_int = (uint32_t)config->scale_x;
  scale_x_frag = (uint32_t)(config->scale_x *
                             PPA_LL_SRM_SCALING_FRAG_MAX) &
                 (PPA_LL_SRM_SCALING_FRAG_MAX - 1);
  scale_y_int = (uint32_t)config->scale_y;
  scale_y_frag = (uint32_t)(config->scale_y *
                             PPA_LL_SRM_SCALING_FRAG_MAX) &
                 (PPA_LL_SRM_SCALING_FRAG_MAX - 1);
  output_mode = esp32p4_ppa_hal_srm_mode(config->output.color_mode);

  ppa_ll_srm_set_rx_color_mode(dev,
    esp32p4_ppa_hal_srm_mode(config->input.color_mode));
  ppa_ll_srm_enable_rx_byte_swap(dev, config->byte_swap != 0);
  ppa_ll_srm_enable_rx_rgb_swap(dev, config->rgb_swap != 0);
  ppa_ll_srm_configure_rx_alpha(dev,
    (ppa_alpha_update_mode_t)config->alpha_mode, config->alpha);
  ppa_ll_srm_set_tx_color_mode(dev, output_mode);
  ppa_ll_srm_set_rotation_angle(dev,
    (ppa_srm_rotation_angle_t)config->rotation);
  ppa_ll_srm_set_scaling_x(dev, scale_x_int, scale_x_frag);
  ppa_ll_srm_set_scaling_y(dev, scale_y_int, scale_y_frag);
  ppa_ll_srm_enable_mirror_x(dev, config->mirror_x != 0);
  ppa_ll_srm_enable_mirror_y(dev, config->mirror_y != 0);

  /* ESP32-P4 erratum DIG-734: keep the ESP-IDF macro-block bypass rule. */

  width_out = config->input.block_width * scale_x_int +
              config->input.block_width * scale_x_frag /
              PPA_LL_SRM_SCALING_FRAG_MAX;
  width_divisor = output_mode == PPA_SRM_COLOR_MODE_ARGB8888 ||
                  output_mode == PPA_SRM_COLOR_MODE_RGB888 ? 32 : 64;
  width_left = width_out % width_divisor;
  width_left = width_left == 0 ? width_divisor : width_left;
  height_in_left = config->input.block_height % 16;
  height_in_left = height_in_left == 0 ? 16 : height_in_left;
  height_left = height_in_left * scale_y_int +
                height_in_left * scale_y_frag /
                PPA_LL_SRM_SCALING_FRAG_MAX;
  output_depth = config->output.color_mode ==
                 ESP32P4_PPA_COLOR_MODE_RGB565 ? 16 :
                 config->output.color_mode ==
                 ESP32P4_PPA_COLOR_MODE_RGB888 ? 24 : 32;
  bypass_mb_order = ((width_out > width_divisor) ||
                     (config->input.block_height > 16)) &&
                    width_left * height_left * output_depth < 12 * 128;
  ppa_ll_srm_bypass_mb_order(dev, bypass_mb_order);
}

void esp32p4_ppa_hal_start_fill(ppa_dev_t *dev)
{
  ppa_ll_blend_start(dev, PPA_LL_BLEND_TRANS_MODE_FILL);
}

void esp32p4_ppa_hal_start_blend(ppa_dev_t *dev)
{
  ppa_ll_blend_start(dev, PPA_LL_BLEND_TRANS_MODE_BLEND);
}

void esp32p4_ppa_hal_start_srm(ppa_dev_t *dev)
{
  ppa_ll_srm_start(dev);
}

int esp32p4_ppa_hal_fill(esp32p4_ppa_handle_t handle,
                         const struct esp32p4_ppa_fill_config_s *config)
{
  return esp32p4_ppa_dma2d_fill(handle, config);
}

int esp32p4_ppa_hal_blend(esp32p4_ppa_handle_t handle,
                          const struct esp32p4_ppa_blend_config_s *config)
{
  return esp32p4_ppa_dma2d_blend(handle, config);
}

int esp32p4_ppa_hal_srm(esp32p4_ppa_handle_t handle,
                        const struct esp32p4_ppa_srm_config_s *config)
{
  return esp32p4_ppa_dma2d_srm(handle, config);
}
