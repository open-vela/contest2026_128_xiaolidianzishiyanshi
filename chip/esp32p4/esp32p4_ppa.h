/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa.h
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

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef struct esp32p4_ppa_client_s *esp32p4_ppa_handle_t;

enum esp32p4_ppa_operation_e
{
  ESP32P4_PPA_OPERATION_SRM = 0,
  ESP32P4_PPA_OPERATION_BLEND,
  ESP32P4_PPA_OPERATION_FILL
};

enum esp32p4_ppa_color_mode_e
{
  ESP32P4_PPA_COLOR_MODE_RGB565 = 0,
  ESP32P4_PPA_COLOR_MODE_RGB888,
  ESP32P4_PPA_COLOR_MODE_ARGB8888,
  ESP32P4_PPA_COLOR_MODE_A8
};

enum esp32p4_ppa_rotation_e
{
  ESP32P4_PPA_ROTATION_0 = 0,
  ESP32P4_PPA_ROTATION_90,
  ESP32P4_PPA_ROTATION_180,
  ESP32P4_PPA_ROTATION_270
};

enum esp32p4_ppa_alpha_mode_e
{
  ESP32P4_PPA_ALPHA_NO_CHANGE = 0,
  ESP32P4_PPA_ALPHA_FIX_VALUE,
  ESP32P4_PPA_ALPHA_SCALE,
  ESP32P4_PPA_ALPHA_INVERT
};

enum esp32p4_ppa_burst_length_e
{
  ESP32P4_PPA_BURST_LENGTH_8 = 8,
  ESP32P4_PPA_BURST_LENGTH_16 = 16,
  ESP32P4_PPA_BURST_LENGTH_32 = 32,
  ESP32P4_PPA_BURST_LENGTH_64 = 64,
  ESP32P4_PPA_BURST_LENGTH_128 = 128
};

struct esp32p4_ppa_client_config_s
{
  enum esp32p4_ppa_operation_e operation;
  enum esp32p4_ppa_burst_length_e burst_length;
};

struct esp32p4_ppa_picture_s
{
  void *buffer;
  size_t buffer_size;
  uint32_t pic_width;
  uint32_t pic_height;
  uint32_t block_width;
  uint32_t block_height;
  uint32_t block_offset_x;
  uint32_t block_offset_y;
  uint32_t stride_bytes;
  enum esp32p4_ppa_color_mode_e color_mode;
};

struct esp32p4_ppa_fill_config_s
{
  struct esp32p4_ppa_picture_s output;
  uint32_t color; /* Raw output pixel value for output.color_mode. */
};

struct esp32p4_ppa_blend_config_s
{
  struct esp32p4_ppa_picture_s foreground;
  struct esp32p4_ppa_picture_s background;
  struct esp32p4_ppa_picture_s output;
  enum esp32p4_ppa_alpha_mode_e foreground_alpha_mode;
  enum esp32p4_ppa_alpha_mode_e background_alpha_mode;
  uint8_t foreground_alpha;  /* Fixed alpha, or scale coefficient / 256. */
  uint8_t background_alpha;  /* Fixed alpha, or scale coefficient / 256. */
  uint32_t foreground_color; /* RGB888 used with an A8 foreground. */
  uint8_t foreground_rgb_swap;
  uint8_t foreground_byte_swap;
  uint8_t background_rgb_swap;
  uint8_t background_byte_swap;
  uint8_t foreground_color_key_enable;
  uint8_t background_color_key_enable;
  uint8_t color_key_reverse;
  uint32_t foreground_color_key_low;
  uint32_t foreground_color_key_high;
  uint32_t background_color_key_low;
  uint32_t background_color_key_high;
  uint32_t color_key_default;
};

struct esp32p4_ppa_srm_config_s
{
  struct esp32p4_ppa_picture_s input;
  struct esp32p4_ppa_picture_s output;
  enum esp32p4_ppa_rotation_e rotation;
  float scale_x;
  float scale_y;
  enum esp32p4_ppa_alpha_mode_e alpha_mode;
  uint8_t alpha;              /* Fixed alpha, or scale coefficient / 256. */
  uint8_t mirror_x;           /* Boolean. */
  uint8_t mirror_y;           /* Boolean. */
  uint8_t rgb_swap;
  uint8_t byte_swap;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp32p4_ppa_register(
  const struct esp32p4_ppa_client_config_s *config,
  esp32p4_ppa_handle_t *handle);
int esp32p4_ppa_unregister(esp32p4_ppa_handle_t handle);
size_t esp32p4_ppa_buffer_alignment(void);
bool esp32p4_ppa_buffer_is_flash(const void *buffer);

int esp32p4_ppa_fill(esp32p4_ppa_handle_t handle,
                     const struct esp32p4_ppa_fill_config_s *config);
int esp32p4_ppa_blend(esp32p4_ppa_handle_t handle,
                      const struct esp32p4_ppa_blend_config_s *config);
int esp32p4_ppa_srm(esp32p4_ppa_handle_t handle,
                    const struct esp32p4_ppa_srm_config_s *config);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_PPA_H */
