/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa.c
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
 * This NuttX-native API is based on the data model of Espressif's
 * upper_hal_ppa driver.  It intentionally does not expose ESP-IDF private
 * types or its FreeRTOS transaction scheduler.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/kmalloc.h>

#include "esp32p4_ppa.h"
#include "esp32p4_ppa_internal.h"
#include "esp_memory_utils.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool
esp32p4_ppa_valid_operation(enum esp32p4_ppa_operation_e operation)
{
  return operation >= ESP32P4_PPA_OPERATION_SRM &&
         operation <= ESP32P4_PPA_OPERATION_FILL;
}

static bool
esp32p4_ppa_valid_burst(enum esp32p4_ppa_burst_length_e burst_length)
{
  return burst_length == ESP32P4_PPA_BURST_LENGTH_8 ||
         burst_length == ESP32P4_PPA_BURST_LENGTH_16 ||
         burst_length == ESP32P4_PPA_BURST_LENGTH_32 ||
         burst_length == ESP32P4_PPA_BURST_LENGTH_64 ||
         burst_length == ESP32P4_PPA_BURST_LENGTH_128;
}

static int esp32p4_ppa_check_handle(esp32p4_ppa_handle_t handle,
                                    enum esp32p4_ppa_operation_e operation)
{
  if (handle == NULL || handle->magic != ESP32P4_PPA_CLIENT_MAGIC ||
      handle->operation != operation)
    {
      return -EINVAL;
    }

  return 0;
}

static int esp32p4_ppa_pixel_size(enum esp32p4_ppa_color_mode_e mode,
                                  size_t *pixel_size)
{
  switch (mode)
    {
      case ESP32P4_PPA_COLOR_MODE_RGB565:
        *pixel_size = 2;
        return 0;
      case ESP32P4_PPA_COLOR_MODE_RGB888:
        *pixel_size = 3;
        return 0;
      case ESP32P4_PPA_COLOR_MODE_ARGB8888:
        *pixel_size = 4;
        return 0;
      case ESP32P4_PPA_COLOR_MODE_A8:
        *pixel_size = 1;
        return 0;
      default:
        return -ENOTSUP;
    }
}

static int
esp32p4_ppa_validate_picture(const struct esp32p4_ppa_picture_s *picture,
                             bool output, bool allow_a8)
{
  size_t pixel_size;
  size_t last_row;
  size_t required;
  uintptr_t end;
  int ret;

  if (picture == NULL || picture->buffer == NULL ||
      picture->pic_width == 0 || picture->pic_height == 0 ||
      picture->block_width == 0 || picture->block_height == 0)
    {
      return -EINVAL;
    }

  ret = esp32p4_ppa_pixel_size(picture->color_mode, &pixel_size);
  if (ret < 0 || (!allow_a8 &&
                  picture->color_mode == ESP32P4_PPA_COLOR_MODE_A8))
    {
      return -ENOTSUP;
    }

  if (picture->pic_width > 0x3fff || picture->pic_height > 0x3fff ||
      picture->block_width > 0x3fff || picture->block_height > 0x3fff ||
      picture->stride_bytes % pixel_size != 0 ||
      picture->stride_bytes / pixel_size > 0x3fff)
    {
      return -EINVAL;
    }

  if (picture->block_offset_x >= picture->pic_width ||
      picture->block_width > picture->pic_width - picture->block_offset_x ||
      picture->block_offset_y >= picture->pic_height ||
      picture->block_height >
        picture->pic_height - picture->block_offset_y ||
      picture->stride_bytes < picture->pic_width * pixel_size)
    {
      return -EINVAL;
    }

  if (__builtin_mul_overflow((size_t)(picture->pic_height - 1),
                             (size_t)picture->stride_bytes, &last_row) ||
      __builtin_mul_overflow((size_t)picture->pic_width, pixel_size,
                             &required) ||
      __builtin_add_overflow(last_row, required, &required) ||
      required > picture->buffer_size ||
      __builtin_add_overflow((uintptr_t)picture->buffer,
                             picture->buffer_size - 1, &end))
    {
      return -EINVAL;
    }

  if (output &&
      ((!esp_ptr_internal(picture->buffer) &&
        !esp_ptr_external_ram(picture->buffer)) ||
       (!esp_ptr_internal((void *)end) &&
        !esp_ptr_external_ram((void *)end))))
    {
      return -EINVAL;
    }

  /* ESP-IDF permits PPA input pictures in internal RAM, PSRAM, or mapped
   * flash.  DMA2D can read all three address spaces, and esp_cache_msync()
   * reports ESP_ERR_NOT_SUPPORTED for read-only mapped flash, which the
   * operation layer intentionally treats as an already coherent input.
   */

  if (!output &&
      ((!esp_ptr_internal(picture->buffer) &&
        !esp_ptr_external_ram(picture->buffer) &&
        !esp_ptr_in_drom(picture->buffer)) ||
       (!esp_ptr_internal((void *)end) &&
        !esp_ptr_external_ram((void *)end) &&
        !esp_ptr_in_drom((void *)end))))
    {
      return -EINVAL;
    }

  if (output &&
      (((uintptr_t)picture->buffer &
        (esp32p4_ppa_buffer_alignment() - 1)) != 0 ||
       (picture->buffer_size &
        (esp32p4_ppa_buffer_alignment() - 1)) != 0))
    {
      return -EINVAL;
    }

  return 0;
}

static bool esp32p4_ppa_valid_alpha(enum esp32p4_ppa_alpha_mode_e mode)
{
  return mode >= ESP32P4_PPA_ALPHA_NO_CHANGE &&
         mode <= ESP32P4_PPA_ALPHA_INVERT;
}

static bool esp32p4_ppa_buffers_overlap(
  const struct esp32p4_ppa_picture_s *first,
  const struct esp32p4_ppa_picture_s *second)
{
  uintptr_t first_start = (uintptr_t)first->buffer;
  uintptr_t second_start = (uintptr_t)second->buffer;
  uintptr_t first_end = first_start + first->buffer_size - 1;
  uintptr_t second_end = second_start + second->buffer_size - 1;

  return first_start <= second_end && second_start <= first_end;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_ppa_register(
  const struct esp32p4_ppa_client_config_s *config,
  esp32p4_ppa_handle_t *handle)
{
  struct esp32p4_ppa_client_s *client;
  int ret;

  if (config == NULL || handle == NULL ||
      !esp32p4_ppa_valid_operation(config->operation) ||
      !esp32p4_ppa_valid_burst(config->burst_length))
    {
      return -EINVAL;
    }

  *handle = NULL;
  client = kmm_zalloc(sizeof(*client));
  if (client == NULL)
    {
      return -ENOMEM;
    }

  client->operation = config->operation;
  client->burst_length = config->burst_length;
  client->magic = ESP32P4_PPA_CLIENT_MAGIC;

  ret = esp32p4_ppa_hw_acquire();
  if (ret < 0)
    {
      client->magic = 0;
      kmm_free(client);
      return ret;
    }

  *handle = client;
  return 0;
}

int esp32p4_ppa_unregister(esp32p4_ppa_handle_t handle)
{
  int ret;

  if (handle == NULL || handle->magic != ESP32P4_PPA_CLIENT_MAGIC)
    {
      return -EINVAL;
    }

  ret = esp32p4_ppa_hw_release();
  if (ret < 0)
    {
      return ret;
    }

  handle->magic = 0;
  kmm_free(handle);
  return 0;
}

int esp32p4_ppa_fill(esp32p4_ppa_handle_t handle,
                     const struct esp32p4_ppa_fill_config_s *config)
{
  int ret = esp32p4_ppa_check_handle(handle, ESP32P4_PPA_OPERATION_FILL);

  if (ret < 0 || config == NULL)
    {
      return -EINVAL;
    }

  ret = esp32p4_ppa_validate_picture(&config->output, true, false);
  if (ret < 0)
    {
      return ret;
    }

  return esp32p4_ppa_hal_fill(handle, config);
}

int esp32p4_ppa_blend(esp32p4_ppa_handle_t handle,
                      const struct esp32p4_ppa_blend_config_s *config)
{
  int ret = esp32p4_ppa_check_handle(handle, ESP32P4_PPA_OPERATION_BLEND);

  if (ret < 0 || config == NULL)
    {
      return -EINVAL;
    }

  ret = esp32p4_ppa_validate_picture(&config->background, false, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_ppa_validate_picture(&config->foreground, false, true);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_ppa_validate_picture(&config->output, true, false);
  if (ret < 0)
    {
      return ret;
    }

  if (config->background.color_mode == ESP32P4_PPA_COLOR_MODE_A8 ||
      config->output.color_mode == ESP32P4_PPA_COLOR_MODE_A8)
    {
      return -ENOTSUP;
    }

  if (!esp32p4_ppa_valid_alpha(config->foreground_alpha_mode) ||
      !esp32p4_ppa_valid_alpha(config->background_alpha_mode) ||
      config->foreground.block_width != config->background.block_width ||
      config->foreground.block_height != config->background.block_height ||
      config->output.block_width != config->foreground.block_width ||
      config->output.block_height != config->foreground.block_height ||
      esp32p4_ppa_buffers_overlap(&config->foreground, &config->output) ||
      esp32p4_ppa_buffers_overlap(&config->background, &config->output))
    {
      return -EINVAL;
    }

  return esp32p4_ppa_hal_blend(handle, config);
}

size_t esp32p4_ppa_buffer_alignment(void)
{
  return CONFIG_ESPRESSIF_CACHE_L1_CACHE_LINE_SIZE >
         CONFIG_ESPRESSIF_CACHE_L2_CACHE_LINE_SIZE ?
         CONFIG_ESPRESSIF_CACHE_L1_CACHE_LINE_SIZE :
         CONFIG_ESPRESSIF_CACHE_L2_CACHE_LINE_SIZE;
}

bool esp32p4_ppa_buffer_is_flash(const void *buffer)
{
  return buffer != NULL && esp_ptr_in_drom(buffer);
}

int esp32p4_ppa_srm(esp32p4_ppa_handle_t handle,
                    const struct esp32p4_ppa_srm_config_s *config)
{
  uint32_t scale_x_q4;
  uint32_t scale_y_q4;
  int ret = esp32p4_ppa_check_handle(handle, ESP32P4_PPA_OPERATION_SRM);

  if (ret < 0 || config == NULL)
    {
      return -EINVAL;
    }

  ret = esp32p4_ppa_validate_picture(&config->input, false, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_ppa_validate_picture(&config->output, true, false);
  if (ret < 0)
    {
      return ret;
    }

  if (config->rotation < ESP32P4_PPA_ROTATION_0 ||
      config->rotation > ESP32P4_PPA_ROTATION_270 ||
      !(config->scale_x >= (1.0f / 16.0f) &&
        config->scale_x < 256.0f) ||
      !(config->scale_y >= (1.0f / 16.0f) &&
        config->scale_y < 256.0f) ||
      !esp32p4_ppa_valid_alpha(config->alpha_mode) ||
      config->mirror_x > 1 || config->mirror_y > 1 ||
      esp32p4_ppa_buffers_overlap(&config->input, &config->output))
    {
      return -EINVAL;
    }

  scale_x_q4 = (uint32_t)(config->scale_x * 16.0f);
  scale_y_q4 = (uint32_t)(config->scale_y * 16.0f);

  if (config->rotation == ESP32P4_PPA_ROTATION_0 ||
      config->rotation == ESP32P4_PPA_ROTATION_180)
    {
      if (config->output.block_width !=
          config->input.block_width * scale_x_q4 / 16 ||
          config->output.block_height !=
          config->input.block_height * scale_y_q4 / 16)
        {
          return -EINVAL;
        }
    }
  else if (config->output.block_width !=
           config->input.block_height * scale_y_q4 / 16 ||
           config->output.block_height !=
           config->input.block_width * scale_x_q4 / 16)
    {
      return -EINVAL;
    }

  return esp32p4_ppa_hal_srm(handle, config);
}
