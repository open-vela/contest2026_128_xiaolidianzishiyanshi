/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/ppa_test_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp32p4_ppa.h"

#define PPA_TEST_GUARD_VALUE 0xd3
#define PPA_TEST_DATA_VALUE  0x5a

struct ppa_test_buffer_s
{
  uint8_t *allocation;
  uint8_t *data;
  size_t size;
  size_t guard_size;
};

struct ppa_test_summary_s
{
  unsigned int pass;
  unsigned int fail;
  unsigned int skip;
};

static struct ppa_test_summary_s g_summary;

static size_t ppa_test_align_up(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

static int ppa_test_buffer_alloc(struct ppa_test_buffer_s *buffer,
                                 size_t requested)
{
  size_t alignment = esp32p4_ppa_buffer_alignment();
  size_t total;

  memset(buffer, 0, sizeof(*buffer));
  buffer->size = ppa_test_align_up(requested, alignment);
  buffer->guard_size = alignment;

  if (buffer->size < requested ||
      __builtin_add_overflow(buffer->size, 2 * alignment, &total))
    {
      return -EOVERFLOW;
    }

  buffer->allocation = memalign(alignment, total);
  if (buffer->allocation == NULL)
    {
      return -ENOMEM;
    }

  buffer->data = buffer->allocation + alignment;
  memset(buffer->allocation, PPA_TEST_GUARD_VALUE, alignment);
  memset(buffer->data, PPA_TEST_DATA_VALUE, buffer->size);
  memset(buffer->data + buffer->size, PPA_TEST_GUARD_VALUE, alignment);
  return 0;
}

static void ppa_test_buffer_free(struct ppa_test_buffer_s *buffer)
{
  free(buffer->allocation);
  memset(buffer, 0, sizeof(*buffer));
}

static void ppa_test_picture_init(
  struct esp32p4_ppa_picture_s *picture,
  const struct ppa_test_buffer_s *buffer,
  uint32_t width, uint32_t height, uint32_t stride,
  enum esp32p4_ppa_color_mode_e color_mode)
{
  memset(picture, 0, sizeof(*picture));
  picture->buffer = buffer->data;
  picture->buffer_size = buffer->size;
  picture->pic_width = width;
  picture->pic_height = height;
  picture->block_width = width;
  picture->block_height = height;
  picture->stride_bytes = stride;
  picture->color_mode = color_mode;
}

static int ppa_test_check_guards(const char *name,
                                 const struct ppa_test_buffer_s *buffer)
{
  size_t i;

  for (i = 0; i < buffer->guard_size; i++)
    {
      if (buffer->allocation[i] != PPA_TEST_GUARD_VALUE)
        {
          printf("FAIL %-24s guard-before[%zu]=0x%02x\n", name, i,
                 buffer->allocation[i]);
          return -EIO;
        }

      if (buffer->data[buffer->size + i] != PPA_TEST_GUARD_VALUE)
        {
          printf("FAIL %-24s guard-after[%zu]=0x%02x\n", name, i,
                 buffer->data[buffer->size + i]);
          return -EIO;
        }
    }

  return 0;
}

static void ppa_test_reference_fill(uint8_t *buffer, uint32_t stride,
                                    uint32_t offset_x, uint32_t offset_y,
                                    uint32_t width, uint32_t height,
                                    uint32_t color, size_t pixel_size)
{
  uint32_t x;
  uint32_t y;
  size_t byte;

  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
        {
          uint8_t *pixel = buffer + (offset_y + y) * stride +
                           (offset_x + x) * pixel_size;

          for (byte = 0; byte < pixel_size; byte++)
            {
              pixel[byte] = (uint8_t)(color >> (byte * 8));
            }
        }
    }
}

static uint16_t ppa_test_rgb565_bilinear(const uint16_t *input,
                                         uint32_t width, uint32_t height,
                                         uint32_t x, uint32_t y,
                                         uint32_t scale_x_q4,
                                         uint32_t scale_y_q4)
{
  uint32_t channels[4][3];
  uint32_t source_x_q8;
  uint32_t source_y_q8;
  uint32_t x0;
  uint32_t x1;
  uint32_t y0;
  uint32_t y1;
  uint32_t fraction_x;
  uint32_t fraction_y;
  uint32_t inverse_x;
  uint32_t inverse_y;
  uint32_t result[3];
  uint32_t i;

  source_x_q8 = ((2 * x + 1) * 128 * 16) / scale_x_q4;
  source_y_q8 = ((2 * y + 1) * 128 * 16) / scale_y_q4;
  source_x_q8 = source_x_q8 > 128 ? source_x_q8 - 128 : 0;
  source_y_q8 = source_y_q8 > 128 ? source_y_q8 - 128 : 0;
  if (source_x_q8 > (width - 1) * 256)
    {
      source_x_q8 = (width - 1) * 256;
    }

  if (source_y_q8 > (height - 1) * 256)
    {
      source_y_q8 = (height - 1) * 256;
    }

  x0 = source_x_q8 >> 8;
  y0 = source_y_q8 >> 8;
  x1 = x0 + 1 < width ? x0 + 1 : x0;
  y1 = y0 + 1 < height ? y0 + 1 : y0;
  fraction_x = source_x_q8 & 0xff;
  fraction_y = source_y_q8 & 0xff;
  inverse_x = 256 - fraction_x;
  inverse_y = 256 - fraction_y;

#define PPA_TEST_UNPACK_RGB565(index, pixel) \
  do \
    { \
      channels[index][0] = ((pixel) >> 11) & 0x1f; \
      channels[index][1] = ((pixel) >> 5) & 0x3f; \
      channels[index][2] = (pixel) & 0x1f; \
    } \
  while (0)

  PPA_TEST_UNPACK_RGB565(0, input[y0 * width + x0]);
  PPA_TEST_UNPACK_RGB565(1, input[y0 * width + x1]);
  PPA_TEST_UNPACK_RGB565(2, input[y1 * width + x0]);
  PPA_TEST_UNPACK_RGB565(3, input[y1 * width + x1]);
#undef PPA_TEST_UNPACK_RGB565

  for (i = 0; i < 3; i++)
    {
      result[i] = (channels[0][i] * inverse_x * inverse_y +
                   channels[1][i] * fraction_x * inverse_y +
                   channels[2][i] * inverse_x * fraction_y +
                   channels[3][i] * fraction_x * fraction_y) >> 16;
    }

  return (uint16_t)((result[0] << 11) | (result[1] << 5) | result[2]);
}

static void ppa_test_print_rgb565_matrix(const char *label,
                                         const uint16_t *pixels,
                                         uint32_t width, uint32_t height)
{
  uint32_t x;
  uint32_t y;

  printf("%s:\n", label);
  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
        {
          printf(" %04x", pixels[y * width + x]);
        }

      printf("\n");
    }
}

static int ppa_test_compare(const char *name, const uint8_t *expected,
                            const uint8_t *actual, size_t size,
                            uint32_t stride, size_t pixel_size)
{
  size_t i;

  for (i = 0; i < size; i++)
    {
      if (expected[i] != actual[i])
        {
          size_t row = i / stride;
          size_t column_byte = i % stride;

          printf("FAIL %-24s byte=%zu x=%zu y=%zu channel=%zu "
                 "expected=0x%02x actual=0x%02x\n",
                 name, i, column_byte / pixel_size, row,
                 column_byte % pixel_size, expected[i], actual[i]);
          return -EIO;
        }
    }

  return 0;
}

static int ppa_test_run_fill(const char *name,
                             enum esp32p4_ppa_color_mode_e mode,
                             size_t pixel_size, uint32_t color,
                             uint32_t pic_width, uint32_t pic_height,
                             uint32_t stride, uint32_t offset_x,
                             uint32_t offset_y, uint32_t block_width,
                             uint32_t block_height)
{
  struct esp32p4_ppa_client_config_s client_config;
  struct esp32p4_ppa_fill_config_s fill_config;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint8_t *expected = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&output, (size_t)stride * pic_height);
  if (ret < 0)
    {
      return ret;
    }

  expected = malloc(output.size);
  if (expected == NULL)
    {
      ppa_test_buffer_free(&output);
      return -ENOMEM;
    }

  memcpy(expected, output.data, output.size);
  ppa_test_reference_fill(expected, stride, offset_x, offset_y,
                          block_width, block_height, color, pixel_size);

  client_config.operation = ESP32P4_PPA_OPERATION_FILL;
  client_config.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client_config, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&fill_config, 0, sizeof(fill_config));
  fill_config.output.buffer = output.data;
  fill_config.output.buffer_size = output.size;
  fill_config.output.pic_width = pic_width;
  fill_config.output.pic_height = pic_height;
  fill_config.output.block_width = block_width;
  fill_config.output.block_height = block_height;
  fill_config.output.block_offset_x = offset_x;
  fill_config.output.block_offset_y = offset_y;
  fill_config.output.stride_bytes = stride;
  fill_config.output.color_mode = mode;
  fill_config.color = color;

  ret = esp32p4_ppa_fill(handle, &fill_config);
  if (ret == 0)
    {
      ret = ppa_test_compare(name, expected, output.data, output.size,
                             stride, pixel_size);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  free(expected);
  ppa_test_buffer_free(&output);
  return ret;
}

static void ppa_test_result(const char *name, int ret)
{
  if (ret == 0)
    {
      g_summary.pass++;
      printf("PASS %-24s\n", name);
    }
  else
    {
      g_summary.fail++;
      printf("FAIL %-24s ret=%d\n", name, ret);
    }
}

static void ppa_test_fill_suite(void)
{
  ppa_test_result("fill-rgb565-full",
                  ppa_test_run_fill("fill-rgb565-full",
                    ESP32P4_PPA_COLOR_MODE_RGB565, 2, 0x0000f81f,
                    16, 16, 40, 0, 0, 16, 16));
  ppa_test_result("fill-rgb565-subrect",
                  ppa_test_run_fill("fill-rgb565-subrect",
                    ESP32P4_PPA_COLOR_MODE_RGB565, 2, 0x000007e0,
                    13, 11, 32, 3, 2, 7, 6));
  ppa_test_result("fill-rgb888-stride",
                  ppa_test_run_fill("fill-rgb888-stride",
                    ESP32P4_PPA_COLOR_MODE_RGB888, 3, 0x00112233,
                    5, 7, 21, 1, 1, 3, 5));
  ppa_test_result("fill-argb8888",
                  ppa_test_run_fill("fill-argb8888",
                    ESP32P4_PPA_COLOR_MODE_ARGB8888, 4, 0xa1b2c3d4,
                    9, 8, 48, 2, 3, 1, 1));
}

static int ppa_test_lifecycle_case(void)
{
  struct esp32p4_ppa_client_config_s config;
  struct esp32p4_ppa_fill_config_s fill;
  esp32p4_ppa_handle_t fill_a = NULL;
  esp32p4_ppa_handle_t fill_b = NULL;
  esp32p4_ppa_handle_t blend = NULL;
  int ret;

  config.operation = ESP32P4_PPA_OPERATION_FILL;
  config.burst_length = ESP32P4_PPA_BURST_LENGTH_32;
  ret = esp32p4_ppa_register(&config, &fill_a);
  if (ret < 0)
    {
      goto out;
    }

  ret = esp32p4_ppa_register(&config, &fill_b);
  if (ret < 0)
    {
      goto out;
    }

  config.operation = ESP32P4_PPA_OPERATION_BLEND;
  ret = esp32p4_ppa_register(&config, &blend);
  if (ret < 0)
    {
      goto out;
    }

  memset(&fill, 0, sizeof(fill));
  ret = esp32p4_ppa_fill(blend, &fill);
  if (ret != -EINVAL)
    {
      ret = -EIO;
      goto out;
    }

  ret = 0;

out:
  if (blend != NULL && esp32p4_ppa_unregister(blend) < 0 && ret == 0)
    {
      ret = -EIO;
    }

  if (fill_b != NULL && esp32p4_ppa_unregister(fill_b) < 0 && ret == 0)
    {
      ret = -EIO;
    }

  if (fill_a != NULL && esp32p4_ppa_unregister(fill_a) < 0 && ret == 0)
    {
      ret = -EIO;
    }

  return ret;
}

static int ppa_test_invalid_case(void)
{
  struct esp32p4_ppa_client_config_s client_config;
  struct esp32p4_ppa_blend_config_s blend;
  struct esp32p4_ppa_fill_config_s fill;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&output, 256);
  if (ret < 0)
    {
      return ret;
    }

  client_config.operation = ESP32P4_PPA_OPERATION_FILL;
  client_config.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client_config, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&fill, 0, sizeof(fill));
  fill.output.buffer = output.data;
  fill.output.buffer_size = output.size;
  fill.output.pic_width = 8;
  fill.output.pic_height = 8;
  fill.output.block_width = 8;
  fill.output.block_height = 8;
  fill.output.stride_bytes = 16;
  fill.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  fill.color = 0x001f;
  memset(&blend, 0, sizeof(blend));
  memset(&srm, 0, sizeof(srm));

#define PPA_EXPECT_INVALID(expression) \
  do \
    { \
      if ((expression) != -EINVAL) \
        { \
          ret = -EIO; \
          goto out; \
        } \
    } \
  while (0)

  PPA_EXPECT_INVALID(esp32p4_ppa_fill(NULL, &fill));
  PPA_EXPECT_INVALID(esp32p4_ppa_fill(handle, NULL));
  PPA_EXPECT_INVALID(esp32p4_ppa_blend(handle, &blend));
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  fill.output.pic_width = 0;
  PPA_EXPECT_INVALID(esp32p4_ppa_fill(handle, &fill));
  fill.output.pic_width = 8;
  fill.output.block_offset_x = 7;
  fill.output.block_width = 2;
  PPA_EXPECT_INVALID(esp32p4_ppa_fill(handle, &fill));
  fill.output.block_offset_x = 0;
  fill.output.block_width = 8;
  fill.output.buffer_size = 64;
  PPA_EXPECT_INVALID(esp32p4_ppa_fill(handle, &fill));
  fill.output.buffer_size = output.size;
  fill.output.buffer = output.data + 1;
  PPA_EXPECT_INVALID(esp32p4_ppa_fill(handle, &fill));
  fill.output.buffer = output.data;

  ret = esp32p4_ppa_fill(handle, &fill);
  if (ret == 0)
    {
      ret = ppa_test_check_guards("invalid-recovery", &output);
    }

#undef PPA_EXPECT_INVALID

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  return ret;
}

static int ppa_test_invalid_accelerators_case(void)
{
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_blend_config_s blend;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input_a;
  struct ppa_test_buffer_s input_b;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&input_a, 8);
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&input_b, 8);
  if (ret < 0)
    {
      ppa_test_buffer_free(&input_a);
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, 8);
  if (ret < 0)
    {
      ppa_test_buffer_free(&input_b);
      ppa_test_buffer_free(&input_a);
      return ret;
    }

  memset(input_a.data, 0x11, input_a.size);
  memset(input_b.data, 0x22, input_b.size);
  memset(output.data, 0, output.size);

#define PPA_EXPECT_INVALID(expression) \
  do \
    { \
      if ((expression) != -EINVAL) \
        { \
          ret = -EIO; \
          goto out; \
        } \
    } \
  while (0)

  client.operation = ESP32P4_PPA_OPERATION_BLEND;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&blend, 0, sizeof(blend));
  ppa_test_picture_init(&blend.background, &input_a, 2, 2, 4,
                        ESP32P4_PPA_COLOR_MODE_RGB565);
  ppa_test_picture_init(&blend.foreground, &input_b, 2, 2, 4,
                        ESP32P4_PPA_COLOR_MODE_RGB565);
  ppa_test_picture_init(&blend.output, &output, 2, 2, 4,
                        ESP32P4_PPA_COLOR_MODE_RGB565);
  blend.background_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

  blend.foreground_alpha_mode =
    (enum esp32p4_ppa_alpha_mode_e)(ESP32P4_PPA_ALPHA_INVERT + 1);
  PPA_EXPECT_INVALID(esp32p4_ppa_blend(handle, &blend));
  blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  blend.foreground.block_width = 1;
  PPA_EXPECT_INVALID(esp32p4_ppa_blend(handle, &blend));
  blend.foreground.block_width = 2;
  blend.output.buffer = input_a.data;
  PPA_EXPECT_INVALID(esp32p4_ppa_blend(handle, &blend));
  blend.output.buffer = output.data;
  ret = esp32p4_ppa_blend(handle, &blend);
  if (ret < 0)
    {
      goto out;
    }

  ret = esp32p4_ppa_unregister(handle);
  handle = NULL;
  if (ret < 0)
    {
      goto out;
    }

  client.operation = ESP32P4_PPA_OPERATION_SRM;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  ppa_test_picture_init(&srm.input, &input_a, 2, 2, 4,
                        ESP32P4_PPA_COLOR_MODE_RGB565);
  ppa_test_picture_init(&srm.output, &output, 2, 2, 4,
                        ESP32P4_PPA_COLOR_MODE_RGB565);
  srm.rotation = ESP32P4_PPA_ROTATION_0;
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

  srm.scale_x = 0.0f;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.scale_x = 256.0f;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.scale_x = NAN;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.scale_x = 1.0f;
  srm.rotation =
    (enum esp32p4_ppa_rotation_e)(ESP32P4_PPA_ROTATION_270 + 1);
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.rotation = ESP32P4_PPA_ROTATION_0;
  srm.mirror_x = 2;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.mirror_x = 0;
  srm.output.block_width = 1;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.output.block_width = 2;
  srm.output.buffer = input_a.data;
  PPA_EXPECT_INVALID(esp32p4_ppa_srm(handle, &srm));
  srm.output.buffer = output.data;
  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0)
    {
      ret = ppa_test_check_guards("invalid-accelerator-input-a",
                                  &input_a);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("invalid-accelerator-input-b",
                                  &input_b);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("invalid-accelerator-output", &output);
    }

#undef PPA_EXPECT_INVALID

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input_b);
  ppa_test_buffer_free(&input_a);
  return ret;
}

static int ppa_test_blend_case(void)
{
  static const uint8_t background_data[12] =
  {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x80, 0x40, 0xa0
  };
  static const uint8_t foreground_data[16] =
  {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x80, 0x80, 0xc0
  };
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_blend_config_s blend;
  struct ppa_test_buffer_s background;
  struct ppa_test_buffer_s foreground;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint8_t *expected = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&background, sizeof(background_data));
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&foreground, sizeof(foreground_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&background);
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, 16);
  if (ret < 0)
    {
      ppa_test_buffer_free(&foreground);
      ppa_test_buffer_free(&background);
      return ret;
    }

  expected = malloc(output.size);
  if (expected == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  memcpy(background.data, background_data, sizeof(background_data));
  memcpy(foreground.data, foreground_data, sizeof(foreground_data));
  memset(output.data, 0xff, 16);
  memcpy(expected, output.data, output.size);
  expected[12] = 0x5b;
  expected[13] = 0x53;
  expected[14] = 0x96;
  expected[15] = 0xd8;

  client.operation = ESP32P4_PPA_OPERATION_BLEND;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&blend, 0, sizeof(blend));
  blend.background.buffer = background.data;
  blend.background.buffer_size = background.size;
  blend.background.pic_width = 2;
  blend.background.pic_height = 2;
  blend.background.block_width = 1;
  blend.background.block_height = 1;
  blend.background.block_offset_x = 1;
  blend.background.block_offset_y = 1;
  blend.background.stride_bytes = 6;
  blend.background.color_mode = ESP32P4_PPA_COLOR_MODE_RGB888;
  blend.foreground.buffer = foreground.data;
  blend.foreground.buffer_size = foreground.size;
  blend.foreground.pic_width = 2;
  blend.foreground.pic_height = 2;
  blend.foreground.block_width = 1;
  blend.foreground.block_height = 1;
  blend.foreground.block_offset_x = 1;
  blend.foreground.block_offset_y = 1;
  blend.foreground.stride_bytes = 8;
  blend.foreground.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  blend.output.buffer = output.data;
  blend.output.buffer_size = output.size;
  blend.output.pic_width = 2;
  blend.output.pic_height = 2;
  blend.output.block_width = 1;
  blend.output.block_height = 1;
  blend.output.block_offset_x = 1;
  blend.output.block_offset_y = 1;
  blend.output.stride_bytes = 8;
  blend.output.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  blend.background_alpha_mode = ESP32P4_PPA_ALPHA_SCALE;
  blend.background_alpha = 204;
  blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_INVERT;

  ret = esp32p4_ppa_blend(handle, &blend);
  if (ret == 0)
    {
      ret = ppa_test_compare("blend-rgb888-argb8888", expected,
                             output.data, output.size, 8, 4);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-background", &background);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-foreground", &foreground);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-output", &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  free(expected);
  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&foreground);
  ppa_test_buffer_free(&background);
  return ret;
}

static int ppa_test_blend_a8_case(void)
{
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_blend_config_s blend;
  struct ppa_test_buffer_s background;
  struct ppa_test_buffer_s foreground;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint16_t *background_pixels;
  uint16_t *output_pixels;
  int ret;

  ret = ppa_test_buffer_alloc(&background, 4);
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&foreground, 2);
  if (ret < 0)
    {
      ppa_test_buffer_free(&background);
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, 4);
  if (ret < 0)
    {
      ppa_test_buffer_free(&foreground);
      ppa_test_buffer_free(&background);
      return ret;
    }

  background_pixels = (uint16_t *)background.data;
  output_pixels = (uint16_t *)output.data;
  background_pixels[0] = 0x001f;
  background_pixels[1] = 0x001f;
  foreground.data[0] = 0x00;
  foreground.data[1] = 0xff;
  memset(output.data, 0, output.size);

  client.operation = ESP32P4_PPA_OPERATION_BLEND;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&blend, 0, sizeof(blend));
  blend.background.buffer = background.data;
  blend.background.buffer_size = background.size;
  blend.background.pic_width = 2;
  blend.background.pic_height = 1;
  blend.background.block_width = 2;
  blend.background.block_height = 1;
  blend.background.stride_bytes = 4;
  blend.background.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  blend.foreground.buffer = foreground.data;
  blend.foreground.buffer_size = foreground.size;
  blend.foreground.pic_width = 2;
  blend.foreground.pic_height = 1;
  blend.foreground.block_width = 2;
  blend.foreground.block_height = 1;
  blend.foreground.stride_bytes = 2;
  blend.foreground.color_mode = ESP32P4_PPA_COLOR_MODE_A8;
  blend.output.buffer = output.data;
  blend.output.buffer_size = output.size;
  blend.output.pic_width = 2;
  blend.output.pic_height = 1;
  blend.output.block_width = 2;
  blend.output.block_height = 1;
  blend.output.stride_bytes = 4;
  blend.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  blend.background_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  blend.foreground_color = 0x00ff0000;

  ret = esp32p4_ppa_blend(handle, &blend);
  if (ret == 0 &&
      (output_pixels[0] != 0x001f || output_pixels[1] != 0xf800))
    {
      printf("FAIL blend-a8-rgb565 pixels=%04x,%04x expected=001f,f800\n",
             output_pixels[0], output_pixels[1]);
      ret = -EIO;
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-a8-background", &background);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-a8-foreground", &foreground);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-a8-output", &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&foreground);
  ppa_test_buffer_free(&background);
  return ret;
}

static int ppa_test_blend_alpha_fix_case(void)
{
  static const uint32_t background_data[2] =
  {
    0xff112233, 0xff445566
  };
  static const uint32_t foreground_data[2] =
  {
    0xffabcdef, 0xff102030
  };
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_blend_config_s blend;
  struct ppa_test_buffer_s background;
  struct ppa_test_buffer_s foreground;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&background, sizeof(background_data));
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&foreground, sizeof(foreground_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&background);
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, sizeof(background_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&foreground);
      ppa_test_buffer_free(&background);
      return ret;
    }

  memcpy(background.data, background_data, sizeof(background_data));
  memcpy(foreground.data, foreground_data, sizeof(foreground_data));
  memset(output.data, 0, output.size);
  client.operation = ESP32P4_PPA_OPERATION_BLEND;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&blend, 0, sizeof(blend));
  blend.background.buffer = background.data;
  blend.background.buffer_size = background.size;
  blend.background.pic_width = 2;
  blend.background.pic_height = 1;
  blend.background.block_width = 2;
  blend.background.block_height = 1;
  blend.background.stride_bytes = 8;
  blend.background.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  blend.foreground.buffer = foreground.data;
  blend.foreground.buffer_size = foreground.size;
  blend.foreground.pic_width = 2;
  blend.foreground.pic_height = 1;
  blend.foreground.block_width = 2;
  blend.foreground.block_height = 1;
  blend.foreground.stride_bytes = 8;
  blend.foreground.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  blend.output.buffer = output.data;
  blend.output.buffer_size = output.size;
  blend.output.pic_width = 2;
  blend.output.pic_height = 1;
  blend.output.block_width = 2;
  blend.output.block_height = 1;
  blend.output.stride_bytes = 8;
  blend.output.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  blend.background_alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  blend.foreground_alpha_mode = ESP32P4_PPA_ALPHA_FIX_VALUE;
  blend.foreground_alpha = 0;

  ret = esp32p4_ppa_blend(handle, &blend);
  if (ret == 0)
    {
      ret = ppa_test_compare("blend-alpha-fix",
                             (const uint8_t *)background_data,
                             output.data, sizeof(background_data), 8, 4);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-alpha-background", &background);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-alpha-foreground", &foreground);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("blend-alpha-output", &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&foreground);
  ppa_test_buffer_free(&background);
  return ret;
}

static int ppa_test_srm_case(void)
{
  static const uint16_t input_data[16] =
  {
    0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0x8080, 0x8080, 0x8080,
    0xffff, 0x8f80, 0x8f80, 0x8f80,
    0xffff, 0xff80, 0xff80, 0xff80
  };
  static const uint16_t expected_data[16] =
  {
    0x0000, 0x8080, 0x8f80, 0xff80,
    0x0000, 0x8080, 0x8f80, 0xff80,
    0x0000, 0x8080, 0x8f80, 0xff80,
    0x0000, 0x0000, 0x0000, 0x0000
  };
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint8_t *expected = NULL;
  int ret;

  ret = ppa_test_buffer_alloc(&input, sizeof(input_data));
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, sizeof(expected_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&input);
      return ret;
    }

  expected = calloc(1, output.size);
  if (expected == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  memcpy(input.data, input_data, sizeof(input_data));
  memset(output.data, 0, output.size);
  memcpy(expected, expected_data, sizeof(expected_data));
  client.operation = ESP32P4_PPA_OPERATION_SRM;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  srm.input.buffer = input.data;
  srm.input.buffer_size = input.size;
  srm.input.pic_width = 4;
  srm.input.pic_height = 4;
  srm.input.block_width = 3;
  srm.input.block_height = 3;
  srm.input.block_offset_x = 1;
  srm.input.block_offset_y = 1;
  srm.input.stride_bytes = 8;
  srm.input.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.output.buffer = output.data;
  srm.output.buffer_size = output.size;
  srm.output.pic_width = 4;
  srm.output.pic_height = 4;
  srm.output.block_width = 3;
  srm.output.block_height = 3;
  srm.output.block_offset_x = 1;
  srm.output.stride_bytes = 8;
  srm.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.rotation = ESP32P4_PPA_ROTATION_90;
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0)
    {
      ret = ppa_test_compare("srm-rgb565-rotate90", expected,
                             output.data, output.size, 8, 2);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("srm-input", &input);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("srm-output", &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  free(expected);
  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input);
  return ret;
}

static int ppa_test_srm_identity_case(
  const char *name, enum esp32p4_ppa_color_mode_e mode,
  size_t pixel_size, bool mirror_x, bool mirror_y)
{
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint8_t *expected = NULL;
  uint32_t stride = 5 * pixel_size + pixel_size;
  uint32_t x;
  uint32_t y;
  size_t byte;
  int ret;

  ret = ppa_test_buffer_alloc(&input, stride * 4);
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, stride * 4);
  if (ret < 0)
    {
      ppa_test_buffer_free(&input);
      return ret;
    }

  expected = malloc(output.size);
  if (expected == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  memset(input.data, 0x33, input.size);
  memset(output.data, 0x5a, output.size);
  memcpy(expected, output.data, output.size);
  for (y = 0; y < 2; y++)
    {
      for (x = 0; x < 3; x++)
        {
          uint32_t value = 0x1021u + y * 0x30201u + x * 0x1711u;
          uint8_t *source = input.data + (y + 1) * stride +
                            (x + 1) * pixel_size;
          uint32_t destination_x = mirror_x ? 2 - x : x;
          uint32_t destination_y = mirror_y ? 1 - y : y;
          uint8_t *destination = expected + (destination_y + 1) * stride +
                                 (destination_x + 1) * pixel_size;

          if (pixel_size == 4)
            {
              value |= 0x80000000u;
            }

          for (byte = 0; byte < pixel_size; byte++)
            {
              source[byte] = (uint8_t)(value >> (byte * 8));
              destination[byte] = source[byte];
            }
        }
    }

  client.operation = ESP32P4_PPA_OPERATION_SRM;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  srm.input.buffer = input.data;
  srm.input.buffer_size = input.size;
  srm.input.pic_width = 5;
  srm.input.pic_height = 4;
  srm.input.block_width = 3;
  srm.input.block_height = 2;
  srm.input.block_offset_x = 1;
  srm.input.block_offset_y = 1;
  srm.input.stride_bytes = stride;
  srm.input.color_mode = mode;
  srm.output.buffer = output.data;
  srm.output.buffer_size = output.size;
  srm.output.pic_width = 5;
  srm.output.pic_height = 4;
  srm.output.block_width = 3;
  srm.output.block_height = 2;
  srm.output.block_offset_x = 1;
  srm.output.block_offset_y = 1;
  srm.output.stride_bytes = stride;
  srm.output.color_mode = mode;
  srm.rotation = ESP32P4_PPA_ROTATION_0;
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;
  srm.mirror_x = mirror_x;
  srm.mirror_y = mirror_y;

  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0)
    {
      ret = ppa_test_compare(name, expected, output.data, output.size,
                             stride, pixel_size);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &input);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  free(expected);
  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input);
  return ret;
}

static int ppa_test_srm_rotation_case(
  const char *name, enum esp32p4_ppa_rotation_e rotation)
{
  static const uint16_t input_data[6] =
  {
    0x0001, 0x0002, 0x0003,
    0x0004, 0x0005, 0x0006
  };
  uint16_t expected_data[6];
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint32_t output_width;
  uint32_t output_height;
  uint32_t source_x;
  uint32_t source_y;
  uint32_t x;
  uint32_t y;
  int ret;

  output_width = rotation == ESP32P4_PPA_ROTATION_90 ||
                 rotation == ESP32P4_PPA_ROTATION_270 ? 2 : 3;
  output_height = rotation == ESP32P4_PPA_ROTATION_90 ||
                  rotation == ESP32P4_PPA_ROTATION_270 ? 3 : 2;

  for (y = 0; y < output_height; y++)
    {
      for (x = 0; x < output_width; x++)
        {
          switch (rotation)
            {
              case ESP32P4_PPA_ROTATION_0:
                source_x = x;
                source_y = y;
                break;
              case ESP32P4_PPA_ROTATION_90:
                source_x = 2 - y;
                source_y = x;
                break;
              case ESP32P4_PPA_ROTATION_180:
                source_x = 2 - x;
                source_y = 1 - y;
                break;
              default:
                source_x = y;
                source_y = 1 - x;
                break;
            }

          expected_data[y * output_width + x] =
            input_data[source_y * 3 + source_x];
        }
    }

  ret = ppa_test_buffer_alloc(&input, sizeof(input_data));
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, sizeof(expected_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&input);
      return ret;
    }

  memcpy(input.data, input_data, sizeof(input_data));
  memset(output.data, 0, output.size);
  client.operation = ESP32P4_PPA_OPERATION_SRM;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  srm.input.buffer = input.data;
  srm.input.buffer_size = input.size;
  srm.input.pic_width = 3;
  srm.input.pic_height = 2;
  srm.input.block_width = 3;
  srm.input.block_height = 2;
  srm.input.stride_bytes = 6;
  srm.input.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.output.buffer = output.data;
  srm.output.buffer_size = output.size;
  srm.output.pic_width = output_width;
  srm.output.pic_height = output_height;
  srm.output.block_width = output_width;
  srm.output.block_height = output_height;
  srm.output.stride_bytes = output_width * 2;
  srm.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.rotation = rotation;
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0)
    {
      ret = ppa_test_compare(name, (const uint8_t *)expected_data,
                             output.data, sizeof(expected_data),
                             output_width * 2, 2);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &input);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input);
  return ret;
}

static int ppa_test_srm_scale_case(const char *name, uint32_t scale_q4)
{
  static const uint16_t downscale_data[16] =
  {
    0xf800, 0x07e0, 0x001f, 0xffff,
    0x0000, 0x7bef, 0xffe0, 0xf81f,
    0x07ff, 0x4208, 0x8410, 0xc618,
    0x18e3, 0x39e7, 0x5aeb, 0xdefb
  };
  static const uint16_t fractional_scale_data[16] =
  {
    0x5aeb, 0x5aeb, 0x5aeb, 0x5aeb,
    0x5aeb, 0x5aeb, 0x5aeb, 0x5aeb,
    0x5aeb, 0x5aeb, 0x5aeb, 0x5aeb,
    0x5aeb, 0x5aeb, 0x5aeb, 0x5aeb
  };
  static const uint16_t upscale_data[4] =
  {
    0xf800, 0x07e0,
    0x001f, 0xffff
  };
  uint16_t expected_data[64];
  const uint16_t *input_data;
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint32_t x;
  uint32_t y;
  uint32_t input_width;
  uint32_t input_height;
  size_t input_bytes;
  uint32_t output_width;
  uint32_t output_height;
  size_t output_bytes;
  int ret;

  if (scale_q4 == 8)
    {
      input_data = downscale_data;
      input_width = 4;
      input_height = 4;
    }
  else if (scale_q4 == 24)
    {
      /* ESP-IDF documents bilinear scaling but not the bit-exact RGB565
       * component rounding.  A constant non-zero image validates the
       * fractional scale and output geometry without inventing a rounding
       * contract that the hardware does not provide.
       */

      input_data = fractional_scale_data;
      input_width = 4;
      input_height = 4;
    }
  else
    {
      /* This 2x four-corner vector has also been checked against the
       * ESP32-P4 rev1 hardware bilinear output matrix.
       */

      input_data = upscale_data;
      input_width = 2;
      input_height = 2;
    }

  input_bytes = input_width * input_height * 2;
  output_width = input_width * scale_q4 / 16;
  output_height = input_height * scale_q4 / 16;
  output_bytes = output_width * output_height * 2;

  for (y = 0; y < output_height; y++)
    {
      for (x = 0; x < output_width; x++)
        {
          expected_data[y * output_width + x] =
            ppa_test_rgb565_bilinear(input_data, input_width, input_height,
                                     x, y,
                                     scale_q4, scale_q4);
        }
    }

  ret = ppa_test_buffer_alloc(&input, input_bytes);
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, output_bytes);
  if (ret < 0)
    {
      ppa_test_buffer_free(&input);
      return ret;
    }

  memcpy(input.data, input_data, input_bytes);
  memset(output.data, 0, output.size);
  client.operation = ESP32P4_PPA_OPERATION_SRM;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  srm.input.buffer = input.data;
  srm.input.buffer_size = input.size;
  srm.input.pic_width = input_width;
  srm.input.pic_height = input_height;
  srm.input.block_width = input_width;
  srm.input.block_height = input_height;
  srm.input.stride_bytes = input_width * 2;
  srm.input.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.output.buffer = output.data;
  srm.output.buffer_size = output.size;
  srm.output.pic_width = output_width;
  srm.output.pic_height = output_height;
  srm.output.block_width = output_width;
  srm.output.block_height = output_height;
  srm.output.stride_bytes = output_width * 2;
  srm.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  srm.rotation = ESP32P4_PPA_ROTATION_0;
  srm.scale_x = (float)scale_q4 / 16.0f;
  srm.scale_y = (float)scale_q4 / 16.0f;
  srm.alpha_mode = ESP32P4_PPA_ALPHA_NO_CHANGE;

  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0 && memcmp(output.data, expected_data, output_bytes) != 0)
    {
      ppa_test_print_rgb565_matrix("SRM_SCALE_EXPECTED", expected_data,
                                   output_width, output_height);
      ppa_test_print_rgb565_matrix("SRM_SCALE_ACTUAL",
                                   (const uint16_t *)output.data,
                                   output_width, output_height);
      ret = ppa_test_compare(name, (const uint8_t *)expected_data,
                             output.data, output_bytes,
                             output_width * 2, 2);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("srm-scale-input", &input);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards("srm-scale-output", &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input);
  return ret;
}

static int ppa_test_srm_alpha_case(
  const char *name, enum esp32p4_ppa_alpha_mode_e alpha_mode,
  uint8_t alpha)
{
  static const uint32_t input_data[4] =
  {
    0x00223344, 0x40223344, 0x80223344, 0xff223344
  };
  uint32_t expected_data[4];
  struct esp32p4_ppa_client_config_s client;
  struct esp32p4_ppa_srm_config_s srm;
  struct ppa_test_buffer_s input;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint32_t input_alpha;
  uint32_t output_alpha;
  size_t i;
  int ret;

  for (i = 0; i < 4; i++)
    {
      input_alpha = input_data[i] >> 24;
      if (alpha_mode == ESP32P4_PPA_ALPHA_FIX_VALUE)
        {
          output_alpha = alpha;
        }
      else if (alpha_mode == ESP32P4_PPA_ALPHA_SCALE)
        {
          output_alpha = input_alpha * alpha >> 8;
        }
      else if (alpha_mode == ESP32P4_PPA_ALPHA_INVERT)
        {
          output_alpha = 255 - input_alpha;
        }
      else
        {
          output_alpha = input_alpha;
        }

      expected_data[i] = (input_data[i] & 0x00ffffff) |
                         (output_alpha << 24);
    }

  ret = ppa_test_buffer_alloc(&input, sizeof(input_data));
  if (ret < 0)
    {
      return ret;
    }

  ret = ppa_test_buffer_alloc(&output, sizeof(expected_data));
  if (ret < 0)
    {
      ppa_test_buffer_free(&input);
      return ret;
    }

  memcpy(input.data, input_data, sizeof(input_data));
  memset(output.data, 0, output.size);
  client.operation = ESP32P4_PPA_OPERATION_SRM;
  client.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&srm, 0, sizeof(srm));
  srm.input.buffer = input.data;
  srm.input.buffer_size = input.size;
  srm.input.pic_width = 4;
  srm.input.pic_height = 1;
  srm.input.block_width = 4;
  srm.input.block_height = 1;
  srm.input.stride_bytes = 16;
  srm.input.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  srm.output.buffer = output.data;
  srm.output.buffer_size = output.size;
  srm.output.pic_width = 4;
  srm.output.pic_height = 1;
  srm.output.block_width = 4;
  srm.output.block_height = 1;
  srm.output.stride_bytes = 16;
  srm.output.color_mode = ESP32P4_PPA_COLOR_MODE_ARGB8888;
  srm.rotation = ESP32P4_PPA_ROTATION_0;
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_mode = alpha_mode;
  srm.alpha = alpha;

  ret = esp32p4_ppa_srm(handle, &srm);
  if (ret == 0)
    {
      ret = ppa_test_compare(name, (const uint8_t *)expected_data,
                             output.data, sizeof(expected_data), 16, 4);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &input);
    }

  if (ret == 0)
    {
      ret = ppa_test_check_guards(name, &output);
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  ppa_test_buffer_free(&input);
  return ret;
}

static void ppa_test_srm_suite(void)
{
  ppa_test_result("srm-rgb565-rotate90", ppa_test_srm_case());
  ppa_test_result("srm-rgb565-rotate0",
                  ppa_test_srm_rotation_case("srm-rgb565-rotate0",
                    ESP32P4_PPA_ROTATION_0));
  ppa_test_result("srm-rgb565-rotate180",
                  ppa_test_srm_rotation_case("srm-rgb565-rotate180",
                    ESP32P4_PPA_ROTATION_180));
  ppa_test_result("srm-rgb565-rotate270",
                  ppa_test_srm_rotation_case("srm-rgb565-rotate270",
                    ESP32P4_PPA_ROTATION_270));
  ppa_test_result("srm-rgb888-mirror-x",
                  ppa_test_srm_identity_case("srm-rgb888-mirror-x",
                    ESP32P4_PPA_COLOR_MODE_RGB888, 3, true, false));
  ppa_test_result("srm-argb8888-mirror-y",
                  ppa_test_srm_identity_case("srm-argb8888-mirror-y",
                    ESP32P4_PPA_COLOR_MODE_ARGB8888, 4, false, true));
  ppa_test_result("srm-rgb565-scale0.5",
                  ppa_test_srm_scale_case("srm-rgb565-scale0.5", 8));
  ppa_test_result("srm-rgb565-scale1.5",
                  ppa_test_srm_scale_case("srm-rgb565-scale1.5", 24));
  ppa_test_result("srm-rgb565-scale2",
                  ppa_test_srm_scale_case("srm-rgb565-scale2", 32));
  ppa_test_result("srm-argb8888-alpha-fix",
                  ppa_test_srm_alpha_case("srm-argb8888-alpha-fix",
                    ESP32P4_PPA_ALPHA_FIX_VALUE, 0xa5));
  ppa_test_result("srm-argb8888-alpha-scale",
                  ppa_test_srm_alpha_case("srm-argb8888-alpha-scale",
                    ESP32P4_PPA_ALPHA_SCALE, 128));
  ppa_test_result("srm-argb8888-alpha-invert",
                  ppa_test_srm_alpha_case("srm-argb8888-alpha-invert",
                    ESP32P4_PPA_ALPHA_INVERT, 0));
}

static int ppa_test_repeat_fill(unsigned long iterations)
{
  struct esp32p4_ppa_client_config_s client_config;
  struct esp32p4_ppa_fill_config_s fill;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint16_t *pixels;
  unsigned long iteration;
  uint32_t color;
  size_t i;
  int ret;

  ret = ppa_test_buffer_alloc(&output, 32 * 16);
  if (ret < 0)
    {
      return ret;
    }

  client_config.operation = ESP32P4_PPA_OPERATION_FILL;
  client_config.burst_length = ESP32P4_PPA_BURST_LENGTH_64;
  ret = esp32p4_ppa_register(&client_config, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&fill, 0, sizeof(fill));
  fill.output.buffer = output.data;
  fill.output.buffer_size = output.size;
  fill.output.pic_width = 16;
  fill.output.pic_height = 16;
  fill.output.block_width = 16;
  fill.output.block_height = 16;
  fill.output.stride_bytes = 32;
  fill.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  pixels = (uint16_t *)output.data;

  for (iteration = 0; iteration < iterations; iteration++)
    {
      color = (iteration & 1) != 0 ? 0xf800 : 0x07e0;
      memset(output.data, (int)(iteration & 0xff), output.size);
      fill.color = color;
#ifdef CONFIG_ESP32P4_PPA_DEBUG
      if (iteration == 0)
        {
          printf("PPA_CACHE_PRE: iteration=%lu buffer=%p pixel0=0x%04x "
                 "expected=0x%04" PRIx32 "\n",
                 iteration, output.data, pixels[0], color);
        }
#endif
      ret = esp32p4_ppa_fill(handle, &fill);
#ifdef CONFIG_ESP32P4_PPA_DEBUG
      if (iteration == 0)
        {
          printf("PPA_CACHE_POST: iteration=%lu buffer=%p ret=%d "
                 "pixel0=0x%04x expected=0x%04" PRIx32 "\n",
                 iteration, output.data, ret, pixels[0], color);
        }
#endif
      if (ret < 0)
        {
          printf("repeat iteration=%lu ret=%d\n", iteration, ret);
          goto out;
        }

      for (i = 0; i < 16 * 16; i++)
        {
          if (pixels[i] != (uint16_t)color)
            {
              printf("repeat iteration=%lu pixel=%zu expected=0x%04" PRIx32
                     " actual=0x%04x\n", iteration, i, color, pixels[i]);
              ret = -EIO;
              goto out;
            }
        }

      ret = ppa_test_check_guards("repeat-fill", &output);
      if (ret < 0)
        {
          goto out;
        }
    }

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  return ret;
}

static uint64_t ppa_test_monotonic_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int ppa_test_perf(uint32_t width, uint32_t height,
                         unsigned long iterations)
{
  struct esp32p4_ppa_client_config_s client_config;
  struct esp32p4_ppa_fill_config_s fill;
  struct ppa_test_buffer_s output;
  esp32p4_ppa_handle_t handle = NULL;
  uint64_t start;
  uint64_t elapsed;
  uint64_t pixels;
  unsigned long i;
  int ret;

  if (width == 0 || height == 0 || width > 0x3fff || height > 0x3fff ||
      width > UINT32_MAX / 2 || iterations == 0)
    {
      return -EINVAL;
    }

  ret = ppa_test_buffer_alloc(&output, (size_t)width * height * 2);
  if (ret < 0)
    {
      return ret;
    }

  client_config.operation = ESP32P4_PPA_OPERATION_FILL;
  client_config.burst_length = ESP32P4_PPA_BURST_LENGTH_128;
  ret = esp32p4_ppa_register(&client_config, &handle);
  if (ret < 0)
    {
      goto out;
    }

  memset(&fill, 0, sizeof(fill));
  fill.output.buffer = output.data;
  fill.output.buffer_size = output.size;
  fill.output.pic_width = width;
  fill.output.pic_height = height;
  fill.output.block_width = width;
  fill.output.block_height = height;
  fill.output.stride_bytes = width * 2;
  fill.output.color_mode = ESP32P4_PPA_COLOR_MODE_RGB565;
  fill.color = 0x07e0;

  for (i = 0; i < 5; i++)
    {
      ret = esp32p4_ppa_fill(handle, &fill);
      if (ret < 0)
        {
          goto out;
        }
    }

  start = ppa_test_monotonic_us();
  for (i = 0; i < iterations; i++)
    {
      ret = esp32p4_ppa_fill(handle, &fill);
      if (ret < 0)
        {
          goto out;
        }
    }

  elapsed = ppa_test_monotonic_us() - start;
  pixels = (uint64_t)width * height * iterations;
  printf("PPA_PERF: op=fill format=rgb565 width=%" PRIu32
         " height=%" PRIu32 " iterations=%lu total_us=%" PRIu64
         " avg_us=%" PRIu64 " pixels_per_s=%" PRIu64 "\n",
         width, height, iterations, elapsed, elapsed / iterations,
         elapsed == 0 ? 0 : pixels * 1000000 / elapsed);

out:
  if (handle != NULL)
    {
      int unregister_ret = esp32p4_ppa_unregister(handle);
      if (ret == 0)
        {
          ret = unregister_ret;
        }
    }

  ppa_test_buffer_free(&output);
  return ret;
}

static void ppa_test_usage(const char *program)
{
  printf("Usage: %s [all|fill|blend|srm|cache|invalid|lifecycle|"
         "stress [iterations]|perf [width] [height] [iterations]]\n",
         program);
}

int main(int argc, char *argv[])
{
  const char *command = argc > 1 ? argv[1] : "all";
  unsigned long iterations;
  uint32_t width;
  uint32_t height;

  memset(&g_summary, 0, sizeof(g_summary));

  if (strcmp(command, "all") == 0)
    {
      ppa_test_result("lifecycle", ppa_test_lifecycle_case());
      ppa_test_fill_suite();
      ppa_test_result("invalid-and-recovery", ppa_test_invalid_case());
      ppa_test_result("invalid-blend-srm-recovery",
                      ppa_test_invalid_accelerators_case());
      ppa_test_result("cache-alternating-100", ppa_test_repeat_fill(100));
      ppa_test_result("blend-rgb888-argb8888", ppa_test_blend_case());
      ppa_test_result("blend-a8-rgb565", ppa_test_blend_a8_case());
      ppa_test_result("blend-alpha-fix", ppa_test_blend_alpha_fix_case());
      ppa_test_srm_suite();
      ppa_test_result("fill-psram-1024x1024",
                      ppa_test_run_fill("fill-psram-1024x1024",
                        ESP32P4_PPA_COLOR_MODE_RGB565, 2, 0x000007e0,
                        1024, 1024, 2048, 0, 0, 1024, 1024));
    }
  else if (strcmp(command, "fill") == 0)
    {
      ppa_test_fill_suite();
    }
  else if (strcmp(command, "blend") == 0 || strcmp(command, "srm") == 0)
    {
      if (strcmp(command, "blend") == 0)
        {
          ppa_test_result("blend-rgb888-argb8888", ppa_test_blend_case());
          ppa_test_result("blend-a8-rgb565", ppa_test_blend_a8_case());
          ppa_test_result("blend-alpha-fix",
                          ppa_test_blend_alpha_fix_case());
        }
      else
        {
          ppa_test_srm_suite();
        }
    }
  else if (strcmp(command, "cache") == 0)
    {
      ppa_test_result("cache-alternating-100", ppa_test_repeat_fill(100));
    }
  else if (strcmp(command, "invalid") == 0)
    {
      ppa_test_result("invalid-and-recovery", ppa_test_invalid_case());
      ppa_test_result("invalid-blend-srm-recovery",
                      ppa_test_invalid_accelerators_case());
    }
  else if (strcmp(command, "lifecycle") == 0)
    {
      ppa_test_result("lifecycle", ppa_test_lifecycle_case());
    }
  else if (strcmp(command, "stress") == 0)
    {
      iterations = argc > 2 ? strtoul(argv[2], NULL, 0) : 1000;
      ppa_test_result("stress-fill", ppa_test_repeat_fill(iterations));
    }
  else if (strcmp(command, "perf") == 0)
    {
      width = argc > 2 ? strtoul(argv[2], NULL, 0) : 800;
      height = argc > 3 ? strtoul(argv[3], NULL, 0) : 480;
      iterations = argc > 4 ? strtoul(argv[4], NULL, 0) : 50;
      ppa_test_result("perf-fill",
                      ppa_test_perf(width, height, iterations));
    }
  else
    {
      ppa_test_usage(argv[0]);
      return EXIT_FAILURE;
    }

  printf("PPA_TEST_SUMMARY: pass=%u fail=%u skip=%u\n",
         g_summary.pass, g_summary.fail, g_summary.skip);
  return g_summary.fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
