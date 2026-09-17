/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/lv_draw_ppa_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application-side ESP32-P4 PPA draw unit for OpenVela LVGL 9.1.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "esp32p4_ppa.h"
#include "lv_draw_ppa_nuttx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LV_DRAW_UNIT_ID_PPA 80
#define LV_DRAW_UNIT_ID_SW  1
#define LV_PPA_SCALE_UNIT   256
#define LV_PPA_SCALE_STEP   16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct lv_draw_ppa_nuttx_buffer_s
{
  uint8_t *data;
  size_t size;
};

struct lv_draw_ppa_nuttx_unit_s
{
  lv_draw_unit_t base;
  esp32p4_ppa_handle_t fill_handle;
  esp32p4_ppa_handle_t srm_handle;
  esp32p4_ppa_handle_t blend_handle;
  struct lv_draw_ppa_nuttx_buffer_s foreground;
  struct lv_draw_ppa_nuttx_buffer_s background;
  struct lv_draw_ppa_nuttx_buffer_s output;
  int operation_result;
  struct lv_draw_ppa_nuttx_stats_s stats;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct lv_draw_ppa_nuttx_unit_s *g_ppa_unit;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t lv_ppa_align_up(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

static bool lv_ppa_get_draw_area(lv_layer_t *layer, lv_draw_task_t *task,
                                 lv_area_t *draw_area)
{
  return _lv_area_intersect(draw_area, &task->area, &task->clip_area) &&
         _lv_area_intersect(draw_area, draw_area, &layer->buf_area);
}

static bool lv_ppa_fill_supported(lv_draw_task_t *task)
{
  lv_draw_fill_dsc_t *dsc;

  if (task->type != LV_DRAW_TASK_TYPE_FILL)
    {
      return false;
    }

  dsc = task->draw_dsc;
  return dsc != NULL && dsc->opa >= LV_OPA_MAX && dsc->radius == 0 &&
         dsc->grad.dir == LV_GRAD_DIR_NONE;
}

static bool lv_ppa_image_supported(lv_draw_task_t *task)
{
  lv_draw_image_dsc_t *dsc;

  if (task->type != LV_DRAW_TASK_TYPE_IMAGE)
    {
      return false;
    }

  dsc = task->draw_dsc;
  return dsc != NULL && dsc->base.layer != NULL &&
         dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB565 &&
         (dsc->header.cf == LV_COLOR_FORMAT_RGB565 ||
          dsc->header.cf == LV_COLOR_FORMAT_RGB888 ||
          dsc->header.cf == LV_COLOR_FORMAT_XRGB8888 ||
          dsc->header.cf == LV_COLOR_FORMAT_ARGB8888 ||
          dsc->header.cf == LV_COLOR_FORMAT_A8) &&
         lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE &&
         dsc->clip_radius == 0 && dsc->bitmap_mask_src == NULL &&
         dsc->sup == NULL && !dsc->tile &&
         dsc->blend_mode == LV_BLEND_MODE_NORMAL &&
         (dsc->recolor_opa <= LV_OPA_MIN ||
          dsc->header.cf == LV_COLOR_FORMAT_A8) &&
         dsc->skew_x == 0 && dsc->skew_y == 0 &&
         dsc->scale_x >= LV_PPA_SCALE_STEP &&
         dsc->scale_y >= LV_PPA_SCALE_STEP &&
         dsc->scale_x < LV_PPA_SCALE_UNIT * LV_PPA_SCALE_UNIT &&
         dsc->scale_y < LV_PPA_SCALE_UNIT * LV_PPA_SCALE_UNIT &&
         (dsc->scale_x % LV_PPA_SCALE_STEP) == 0 &&
         (dsc->scale_y % LV_PPA_SCALE_STEP) == 0 &&
         (dsc->rotation % 900) == 0 &&
         (dsc->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) == 0;
}

static enum esp32p4_ppa_color_mode_e
lv_ppa_color_mode(lv_color_format_t color_format)
{
  if (color_format == LV_COLOR_FORMAT_RGB565)
    {
      return ESP32P4_PPA_COLOR_MODE_RGB565;
    }

  if (color_format == LV_COLOR_FORMAT_RGB888)
    {
      return ESP32P4_PPA_COLOR_MODE_RGB888;
    }

  if (color_format == LV_COLOR_FORMAT_A8)
    {
      return ESP32P4_PPA_COLOR_MODE_A8;
    }

  return ESP32P4_PPA_COLOR_MODE_ARGB8888;
}

static uint32_t lv_ppa_pixel_size(lv_color_format_t color_format)
{
  return (uint32_t)lv_color_format_get_size(color_format);
}

static int lv_ppa_buffer_size(uint32_t width, uint32_t height,
                              uint32_t pixel_size, size_t *size)
{
  size_t row_size;

  if (__builtin_mul_overflow((size_t)width, (size_t)pixel_size,
                             &row_size) ||
      __builtin_mul_overflow(row_size, (size_t)height, size))
    {
      return -EOVERFLOW;
    }

  return 0;
}

static void lv_ppa_picture_init(struct esp32p4_ppa_picture_s *picture,
                                void *buffer, size_t buffer_size,
                                uint32_t width, uint32_t height,
                                uint32_t stride,
                                enum esp32p4_ppa_color_mode_e color_mode)
{
  memset(picture, 0, sizeof(*picture));
  picture->buffer = buffer;
  picture->buffer_size = buffer_size;
  picture->pic_width = width;
  picture->pic_height = height;
  picture->block_width = width;
  picture->block_height = height;
  picture->stride_bytes = stride;
  picture->color_mode = color_mode;
}

static enum esp32p4_ppa_rotation_e lv_ppa_rotation(int32_t angle)
{
  angle %= 3600;
  if (angle < 0)
    {
      angle += 3600;
    }

  /* LVGL's positive rotation is clockwise in screen coordinates, while
   * the PPA rotation enum is counter-clockwise.
   */

  switch (angle)
    {
      case 900:
        return ESP32P4_PPA_ROTATION_270;
      case 1800:
        return ESP32P4_PPA_ROTATION_180;
      case 2700:
        return ESP32P4_PPA_ROTATION_90;
      default:
        return ESP32P4_PPA_ROTATION_0;
    }
}

static bool lv_ppa_is_transform(const lv_draw_image_dsc_t *dsc)
{
  return (dsc->rotation % 3600) != 0 ||
         dsc->scale_x != LV_PPA_SCALE_UNIT ||
         dsc->scale_y != LV_PPA_SCALE_UNIT;
}

static void lv_ppa_copy_from_target(const lv_draw_buf_t *target,
                                    uint32_t target_x, uint32_t target_y,
                                    uint8_t *compact, uint32_t width,
                                    uint32_t height)
{
  const uint8_t *source = target->data +
                          target_y * target->header.stride + target_x * 2;

  for (uint32_t row = 0; row < height; row++)
    {
      memcpy(compact + row * width * 2,
             source + row * target->header.stride, width * 2);
    }
}

static void lv_ppa_copy_to_target(lv_draw_buf_t *target,
                                  uint32_t target_x, uint32_t target_y,
                                  const uint8_t *compact, uint32_t width,
                                  uint32_t height)
{
  uint8_t *destination = target->data +
                         target_y * target->header.stride + target_x * 2;

  for (uint32_t row = 0; row < height; row++)
    {
      memcpy(destination + row * target->header.stride,
             compact + row * width * 2, width * 2);
    }
}

static int lv_ppa_ensure_buffer(struct lv_draw_ppa_nuttx_buffer_s *buffer,
                                size_t required)
{
  size_t alignment = esp32p4_ppa_buffer_alignment();
  size_t alloc_size = lv_ppa_align_up(required, alignment);
  uint8_t *data;

  if (alloc_size < required)
    {
      return -EOVERFLOW;
    }

  if (buffer->data != NULL && buffer->size >= alloc_size)
    {
      return 0;
    }

  data = memalign(alignment, alloc_size);
  if (data == NULL)
    {
      return -ENOMEM;
    }

  free(buffer->data);
  buffer->data = data;
  buffer->size = alloc_size;
  return 0;
}

static int lv_ppa_fill(struct lv_draw_ppa_nuttx_unit_s *unit,
                       lv_layer_t *layer, lv_draw_task_t *task)
{
  struct esp32p4_ppa_fill_config_s config;
  lv_draw_fill_dsc_t *dsc = task->draw_dsc;
  lv_draw_buf_t *draw_buf = layer->draw_buf;
  lv_area_t area;
  size_t alignment = esp32p4_ppa_buffer_alignment();
  size_t required;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint8_t *destination;
  int ret;

  if (draw_buf == NULL || layer->color_format != LV_COLOR_FORMAT_RGB565 ||
      !lv_ppa_get_draw_area(layer, task, &area))
    {
      return -ENOTSUP;
    }

  width = (uint32_t)lv_area_get_width(&area);
  height = (uint32_t)lv_area_get_height(&area);
  stride = draw_buf->header.stride;
  destination = draw_buf->data;

  memset(&config, 0, sizeof(config));
  config.color = lv_color_to_u16(dsc->color);
  config.output.buffer = destination;
  config.output.buffer_size = draw_buf->data_size;
  config.output.pic_width = draw_buf->header.w;
  config.output.pic_height = draw_buf->header.h;
  config.output.block_width = width;
  config.output.block_height = height;
  config.output.block_offset_x = (uint32_t)(area.x1 - layer->buf_area.x1);
  config.output.block_offset_y = (uint32_t)(area.y1 - layer->buf_area.y1);
  config.output.stride_bytes = stride;
  config.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;

  if (((uintptr_t)destination & (alignment - 1)) == 0 &&
      (draw_buf->data_size & (alignment - 1)) == 0)
    {
      ret = esp32p4_ppa_fill(unit->fill_handle, &config);
      if (ret >= 0)
        {
          unit->stats.fills++;
          unit->stats.direct++;
        }

      return ret;
    }

  if (__builtin_mul_overflow((size_t)width, 2u, &required) ||
      __builtin_mul_overflow(required, (size_t)height, &required))
    {
      return -EOVERFLOW;
    }

  ret = lv_ppa_ensure_buffer(&unit->output, required);
  if (ret < 0)
    {
      return ret;
    }

  config.output.buffer = unit->output.data;
  config.output.buffer_size = unit->output.size;
  config.output.pic_width = width;
  config.output.pic_height = height;
  config.output.block_offset_x = 0;
  config.output.block_offset_y = 0;
  config.output.stride_bytes = width * 2;

  ret = esp32p4_ppa_fill(unit->fill_handle, &config);
  if (ret < 0)
    {
      return ret;
    }

  destination += (area.y1 - layer->buf_area.y1) * stride +
                 (area.x1 - layer->buf_area.x1) * 2;
  for (uint32_t row = 0; row < height; row++)
    {
      memcpy(destination + row * stride,
             unit->output.data + row * width * 2, width * 2);
    }

  unit->stats.fills++;
  unit->stats.staged++;
  return 0;
}

static void lv_ppa_image_core(lv_draw_unit_t *draw_unit,
                              const lv_draw_image_dsc_t *draw_dsc,
                              const lv_image_decoder_dsc_t *decoder_dsc,
                              lv_draw_image_sup_t *sup,
                              const lv_area_t *image_area,
                              const lv_area_t *clipped_area)
{
  struct lv_draw_ppa_nuttx_unit_s *unit =
    (struct lv_draw_ppa_nuttx_unit_s *)draw_unit;
  struct esp32p4_ppa_srm_config_s srm;
  struct esp32p4_ppa_blend_config_s blend;
  const lv_draw_buf_t *source = decoder_dsc->decoded;
  lv_draw_buf_t *target = draw_unit->target_layer->draw_buf;
  struct esp32p4_ppa_picture_s foreground;
  lv_area_t transformed_area;
  lv_area_t output_area;
  lv_color_format_t foreground_format;
  enum esp32p4_ppa_rotation_e rotation;
  uint32_t foreground_width;
  uint32_t foreground_height;
  uint32_t foreground_pixel_size;
  uint32_t width;
  uint32_t height;
  uint32_t target_x;
  uint32_t target_y;
  size_t required;
  bool transform;
  bool needs_blend;
  int ret;

  (void)sup;

  if (source == NULL || target == NULL || source->data == NULL ||
      source->header.w == 0 || source->header.h == 0 ||
      (source->header.cf != LV_COLOR_FORMAT_RGB565 &&
       source->header.cf != LV_COLOR_FORMAT_RGB888 &&
       source->header.cf != LV_COLOR_FORMAT_XRGB8888 &&
       source->header.cf != LV_COLOR_FORMAT_ARGB8888 &&
       source->header.cf != LV_COLOR_FORMAT_A8) ||
      (source->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) != 0)
    {
      unit->operation_result = -ENOTSUP;
      return;
    }

  transform = lv_ppa_is_transform(draw_dsc);
  foreground_format = source->header.cf;
  transformed_area = *image_area;
  foreground_width = source->header.w;
  foreground_height = source->header.h;

  if (esp32p4_ppa_buffer_is_flash(source->data))
    {
      unit->stats.flash_inputs++;
    }

  if (transform)
    {
      uint32_t scaled_width =
        source->header.w * (uint32_t)draw_dsc->scale_x /
        LV_PPA_SCALE_UNIT;
      uint32_t scaled_height =
        source->header.h * (uint32_t)draw_dsc->scale_y /
        LV_PPA_SCALE_UNIT;

      if (foreground_format == LV_COLOR_FORMAT_A8)
        {
          unit->operation_result = -ENOTSUP;
          return;
        }

      rotation = lv_ppa_rotation(draw_dsc->rotation);
      if (rotation == ESP32P4_PPA_ROTATION_90 ||
          rotation == ESP32P4_PPA_ROTATION_270)
        {
          foreground_width = scaled_height;
          foreground_height = scaled_width;
        }
      else
        {
          foreground_width = scaled_width;
          foreground_height = scaled_height;
        }

      if (foreground_width == 0 || foreground_height == 0 ||
          foreground_width > 0x3fff || foreground_height > 0x3fff)
        {
          unit->operation_result = -ENOTSUP;
          return;
        }

      _lv_image_buf_get_transformed_area(&transformed_area,
                                         source->header.w,
                                         source->header.h,
                                         draw_dsc->rotation,
                                         draw_dsc->scale_x,
                                         draw_dsc->scale_y,
                                         &draw_dsc->pivot);
      lv_area_move(&transformed_area, image_area->x1, image_area->y1);
      if ((uint32_t)lv_area_get_width(&transformed_area) !=
          foreground_width ||
          (uint32_t)lv_area_get_height(&transformed_area) !=
          foreground_height)
        {
          unit->operation_result = -ENOTSUP;
          return;
        }

      foreground_pixel_size = lv_ppa_pixel_size(foreground_format);
      ret = lv_ppa_buffer_size(foreground_width, foreground_height,
                               foreground_pixel_size, &required);
      if (ret < 0)
        {
          unit->operation_result = ret;
          return;
        }

      ret = lv_ppa_ensure_buffer(&unit->foreground, required);
      if (ret < 0)
        {
          unit->operation_result = ret;
          return;
        }

      memset(&srm, 0, sizeof(srm));
      lv_ppa_picture_init(&srm.input, (void *)source->data,
                          source->data_size, source->header.w,
                          source->header.h, source->header.stride,
                          lv_ppa_color_mode(foreground_format));
      lv_ppa_picture_init(&srm.output, unit->foreground.data,
                          unit->foreground.size, foreground_width,
                          foreground_height,
                          foreground_width * foreground_pixel_size,
                          lv_ppa_color_mode(foreground_format));
      srm.rotation = rotation;
      srm.scale_x = (float)draw_dsc->scale_x / LV_PPA_SCALE_UNIT;
      srm.scale_y = (float)draw_dsc->scale_y / LV_PPA_SCALE_UNIT;
      srm.alpha_mode = foreground_format == LV_COLOR_FORMAT_XRGB8888 ?
                       ESP32P4_PPA_ALPHA_FIX_VALUE :
                       ESP32P4_PPA_ALPHA_NO_CHANGE;
      srm.alpha = LV_OPA_MAX;
      ret = esp32p4_ppa_srm(unit->srm_handle, &srm);
      if (ret < 0)
        {
          unit->operation_result = ret;
          return;
        }

      lv_ppa_picture_init(&foreground, unit->foreground.data,
                          unit->foreground.size, foreground_width,
                          foreground_height,
                          foreground_width * foreground_pixel_size,
                          lv_ppa_color_mode(foreground_format));
      unit->stats.transforms++;
      unit->stats.staged++;
    }
  else
    {
      lv_ppa_picture_init(&foreground, (void *)source->data,
                          source->data_size, source->header.w,
                          source->header.h, source->header.stride,
                          lv_ppa_color_mode(foreground_format));
    }

  output_area = *clipped_area;
  if (!_lv_area_intersect(&output_area, &output_area,
                          &draw_unit->target_layer->buf_area) ||
      !_lv_area_intersect(&output_area, &output_area, &transformed_area))
    {
      unit->operation_result = 0;
      return;
    }

  width = (uint32_t)lv_area_get_width(&output_area);
  height = (uint32_t)lv_area_get_height(&output_area);
  target_x = (uint32_t)(output_area.x1 -
                        draw_unit->target_layer->buf_area.x1);
  target_y = (uint32_t)(output_area.y1 -
                        draw_unit->target_layer->buf_area.y1);
  foreground.block_width = width;
  foreground.block_height = height;
  foreground.block_offset_x =
    (uint32_t)(output_area.x1 - transformed_area.x1);
  foreground.block_offset_y =
    (uint32_t)(output_area.y1 - transformed_area.y1);

  needs_blend = foreground_format == LV_COLOR_FORMAT_ARGB8888 ||
                foreground_format == LV_COLOR_FORMAT_A8 ||
                draw_dsc->opa < LV_OPA_MAX || draw_dsc->colorkey != NULL;

  ret = lv_ppa_buffer_size(width, height, 2, &required);
  if (ret < 0 || (ret = lv_ppa_ensure_buffer(&unit->output, required)) < 0)
    {
      unit->operation_result = ret;
      return;
    }

  if (!needs_blend)
    {
      memset(&srm, 0, sizeof(srm));
      srm.input = foreground;
      lv_ppa_picture_init(&srm.output, unit->output.data,
                          unit->output.size, width, height, width * 2,
                          ESP32P4_PPA_COLOR_MODE_RGB565);
      srm.rotation = ESP32P4_PPA_ROTATION_0;
      srm.scale_x = 1.0f;
      srm.scale_y = 1.0f;
      srm.alpha_mode = foreground_format == LV_COLOR_FORMAT_XRGB8888 ?
                       ESP32P4_PPA_ALPHA_FIX_VALUE :
                       ESP32P4_PPA_ALPHA_NO_CHANGE;
      srm.alpha = LV_OPA_MAX;

      ret = esp32p4_ppa_srm(unit->srm_handle, &srm);
      if (ret >= 0)
        {
          lv_ppa_copy_to_target(target, target_x, target_y,
                                unit->output.data, width, height);
          unit->stats.copies++;
          unit->stats.staged++;
        }
    }
  else
    {
      ret = lv_ppa_ensure_buffer(&unit->background, required);
      if (ret < 0)
        {
          unit->operation_result = ret;
          return;
        }

      lv_ppa_copy_from_target(target, target_x, target_y,
                              unit->background.data, width, height);
      memset(&blend, 0, sizeof(blend));
      blend.foreground = foreground;
      lv_ppa_picture_init(&blend.background, unit->background.data,
                          unit->background.size, width, height, width * 2,
                          ESP32P4_PPA_COLOR_MODE_RGB565);
      lv_ppa_picture_init(&blend.output, unit->output.data,
                          unit->output.size, width, height, width * 2,
                          ESP32P4_PPA_COLOR_MODE_RGB565);
      blend.background_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

      if (foreground_format == LV_COLOR_FORMAT_ARGB8888 ||
          foreground_format == LV_COLOR_FORMAT_A8)
        {
          blend.foreground_alpha_mode = draw_dsc->opa < LV_OPA_MAX ?
            ESP32P4_PPA_ALPHA_SCALE : ESP32P4_PPA_ALPHA_NO_CHANGE;
          blend.foreground_alpha = draw_dsc->opa;
        }
      else
        {
          blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_FIX_VALUE;
          blend.foreground_alpha = draw_dsc->opa;
        }

      if (foreground_format == LV_COLOR_FORMAT_A8)
        {
          blend.foreground_color = lv_color_to_u32(draw_dsc->recolor);
        }

      if (draw_dsc->colorkey != NULL)
        {
          blend.foreground_color_key_enable = 1;
          blend.foreground_color_key_low =
            lv_color_to_u32(draw_dsc->colorkey->low) & 0x00ffffff;
          blend.foreground_color_key_high =
            lv_color_to_u32(draw_dsc->colorkey->high) & 0x00ffffff;
        }

      ret = esp32p4_ppa_blend(unit->blend_handle, &blend);
      if (ret >= 0)
        {
          lv_ppa_copy_to_target(target, target_x, target_y,
                                unit->output.data, width, height);
          unit->stats.blends++;
          unit->stats.staged++;
        }
    }

  unit->operation_result = ret;
}

static int lv_ppa_image(struct lv_draw_ppa_nuttx_unit_s *unit,
                        lv_draw_task_t *task)
{
  lv_draw_image_dsc_t *dsc = task->draw_dsc;

  unit->operation_result = -EIO;
  _lv_draw_image_normal_helper(&unit->base, dsc, &task->area,
                               lv_ppa_image_core);
  return unit->operation_result;
}

static int32_t lv_ppa_evaluate(lv_draw_unit_t *draw_unit,
                               lv_draw_task_t *task)
{
  (void)draw_unit;

  if ((lv_ppa_fill_supported(task) || lv_ppa_image_supported(task)) &&
      task->preference_score > 80)
    {
      task->preference_score = 80;
      task->preferred_draw_unit_id = LV_DRAW_UNIT_ID_PPA;
    }

  return 0;
}

static int32_t lv_ppa_dispatch(lv_draw_unit_t *draw_unit, lv_layer_t *layer)
{
  struct lv_draw_ppa_nuttx_unit_s *unit =
    (struct lv_draw_ppa_nuttx_unit_s *)draw_unit;
  lv_draw_task_t *task;
  int ret;

  task = lv_draw_get_next_available_task(layer, NULL, LV_DRAW_UNIT_ID_PPA);
  if (task == NULL)
    {
      return -1;
    }

  if (lv_draw_layer_alloc_buf(layer) == NULL)
    {
      return -1;
    }

  task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
  draw_unit->target_layer = layer;
  draw_unit->clip_area = &task->clip_area;

  if (task->type == LV_DRAW_TASK_TYPE_FILL)
    {
      ret = lv_ppa_fill(unit, layer, task);
    }
  else
    {
      ret = lv_ppa_image(unit, task);
    }

  if (ret < 0)
    {
      unit->stats.fallback++;
      task->preferred_draw_unit_id = LV_DRAW_UNIT_ID_SW;
      task->preference_score = 100;
      task->state = LV_DRAW_TASK_STATE_QUEUED;
      lv_draw_dispatch_request();
      return -1;
    }

  unit->stats.accelerated++;
  task->state = LV_DRAW_TASK_STATE_READY;
  lv_draw_dispatch_request();
  return 1;
}

static int32_t lv_ppa_delete(lv_draw_unit_t *draw_unit)
{
  struct lv_draw_ppa_nuttx_unit_s *unit =
    (struct lv_draw_ppa_nuttx_unit_s *)draw_unit;

  free(unit->foreground.data);
  free(unit->background.data);
  free(unit->output.data);
  unit->foreground.data = NULL;
  unit->background.data = NULL;
  unit->output.data = NULL;
  unit->foreground.size = 0;
  unit->background.size = 0;
  unit->output.size = 0;

  if (unit->fill_handle != NULL)
    {
      esp32p4_ppa_unregister(unit->fill_handle);
      unit->fill_handle = NULL;
    }

  if (unit->srm_handle != NULL)
    {
      esp32p4_ppa_unregister(unit->srm_handle);
      unit->srm_handle = NULL;
    }

  if (unit->blend_handle != NULL)
    {
      esp32p4_ppa_unregister(unit->blend_handle);
      unit->blend_handle = NULL;
    }

  if (g_ppa_unit == unit)
    {
      g_ppa_unit = NULL;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int lv_draw_ppa_nuttx_init(void)
{
  struct esp32p4_ppa_client_config_s config;
  struct lv_draw_ppa_nuttx_unit_s *unit;
  esp32p4_ppa_handle_t fill_handle;
  esp32p4_ppa_handle_t srm_handle;
  esp32p4_ppa_handle_t blend_handle;
  int ret;

  if (!lv_is_initialized())
    {
      return -EPERM;
    }

  if (g_ppa_unit != NULL)
    {
      return -EALREADY;
    }

  memset(&config, 0, sizeof(config));
  config.operation = ESP32P4_PPA_OPERATION_FILL;
  config.burst_length = ESP32P4_PPA_BURST_LENGTH_128;

  ret = esp32p4_ppa_register(&config, &fill_handle);
  if (ret < 0)
    {
      return ret;
    }

  config.operation = ESP32P4_PPA_OPERATION_SRM;
  ret = esp32p4_ppa_register(&config, &srm_handle);
  if (ret < 0)
    {
      esp32p4_ppa_unregister(fill_handle);
      return ret;
    }

  config.operation = ESP32P4_PPA_OPERATION_BLEND;
  ret = esp32p4_ppa_register(&config, &blend_handle);
  if (ret < 0)
    {
      esp32p4_ppa_unregister(srm_handle);
      esp32p4_ppa_unregister(fill_handle);
      return ret;
    }

  unit = lv_draw_create_unit(sizeof(*unit));
  if (unit == NULL)
    {
      esp32p4_ppa_unregister(blend_handle);
      esp32p4_ppa_unregister(srm_handle);
      esp32p4_ppa_unregister(fill_handle);
      return -ENOMEM;
    }

  unit->fill_handle = fill_handle;
  unit->srm_handle = srm_handle;
  unit->blend_handle = blend_handle;
  unit->base.name = "PPA";
  unit->base.evaluate_cb = lv_ppa_evaluate;
  unit->base.dispatch_cb = lv_ppa_dispatch;
  unit->base.delete_cb = lv_ppa_delete;
  g_ppa_unit = unit;
  return 0;
}

int lv_draw_ppa_nuttx_get_stats(struct lv_draw_ppa_nuttx_stats_s *stats)
{
  if (stats == NULL)
    {
      return -EINVAL;
    }

  if (g_ppa_unit == NULL)
    {
      return -ENODEV;
    }

  *stats = g_ppa_unit->stats;
  return 0;
}
