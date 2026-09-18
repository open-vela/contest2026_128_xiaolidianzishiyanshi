/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/lvgl_ppa_demo_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "lv_draw_ppa_nuttx.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This const image is kept in mapped flash.  Its asymmetric arrow makes
 * rotation direction visible, while cyan is removed by the color key.
 */

static const uint16_t g_flash_rgb565_pixels[8 * 8] =
{
  0x07ff, 0x07ff, 0x07ff, 0xf800, 0x07ff, 0x07ff, 0x07ff, 0x07ff,
  0x07ff, 0x07ff, 0xf800, 0xf800, 0xf800, 0x07ff, 0x07ff, 0x07ff,
  0x07ff, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0x07ff, 0x07ff,
  0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0xf800, 0x07ff,
  0x07ff, 0x07ff, 0x07e0, 0x07e0, 0x07e0, 0x07ff, 0x07ff, 0x07ff,
  0x07ff, 0x07ff, 0x07e0, 0x07e0, 0x07e0, 0x07ff, 0x07ff, 0x07ff,
  0x07ff, 0x07ff, 0x07e0, 0x07e0, 0x07e0, 0x07ff, 0x07ff, 0x07ff,
  0x07ff, 0x07ff, 0x07ff, 0x07ff, 0x07ff, 0x07ff, 0x07ff, 0x07ff
};

static const lv_image_dsc_t g_flash_rgb565_image =
{
  .header =
  {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_RGB565,
    .w = 8,
    .h = 8,
    .stride = 16
  },
  .data_size = sizeof(g_flash_rgb565_pixels),
  .data = (const uint8_t *)g_flash_rgb565_pixels
};

static lv_color32_t g_ram_argb_pixels[32 * 32];
static uint8_t g_ram_a8_pixels[32 * 32];

static lv_image_dsc_t g_ram_argb_image =
{
  .header =
  {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_ARGB8888,
    .w = 32,
    .h = 32,
    .stride = 32 * 4
  },
  .data_size = sizeof(g_ram_argb_pixels),
  .data = (const uint8_t *)g_ram_argb_pixels
};

static lv_image_dsc_t g_ram_a8_image =
{
  .header =
  {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_A8,
    .w = 32,
    .h = 32,
    .stride = 32
  },
  .data_size = sizeof(g_ram_a8_pixels),
  .data = g_ram_a8_pixels
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void lvgl_ppa_init_ram_images(void)
{
  for (uint32_t y = 0; y < 32; y++)
    {
      for (uint32_t x = 0; x < 32; x++)
        {
          uint32_t distance = (x > 15 ? x - 15 : 15 - x) +
                              (y > 15 ? y - 15 : 15 - y);
          lv_color32_t *pixel = &g_ram_argb_pixels[y * 32 + x];

          pixel->red = 255;
          pixel->green = (uint8_t)(64 + x * 5);
          pixel->blue = (uint8_t)(255 - y * 5);
          pixel->alpha = distance >= 24 ? 0 :
                         (uint8_t)(255 - distance * 10);
          g_ram_a8_pixels[y * 32 + x] =
            (uint8_t)(((x ^ y) & 8) != 0 ? 224 : 72);
        }
    }
}

static void lvgl_ppa_create_ui(void)
{
  static const lv_image_colorkey_t flash_key =
  {
    .low = {.blue = 0xff, .green = 0xff, .red = 0x00},
    .high = {.blue = 0xff, .green = 0xff, .red = 0x00}
  };

  static const uint32_t colors[] =
  {
    0xe53935, 0x43a047, 0x1e88e5, 0xfdd835
  };

  lv_obj_t *screen = lv_screen_active();

  lvgl_ppa_init_ram_images();

  lv_obj_set_style_bg_color(screen, lv_color_hex(0x20242a), 0);

  for (unsigned int index = 0; index < 4; index++)
    {
      lv_obj_t *panel = lv_obj_create(screen);
      lv_obj_remove_style_all(panel);
      lv_obj_set_size(panel, 120, 80);
      lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(panel, lv_color_hex(colors[index]), 0);
      lv_obj_align(panel, LV_ALIGN_CENTER,
                   (index & 1) ? 70 : -70,
                   (index & 2) ? 150 : 55);
    }

  lv_obj_t *flash_transform = lv_image_create(screen);
  lv_image_set_src(flash_transform, &g_flash_rgb565_image);
  lv_image_set_pivot(flash_transform, 4, 4);
  lv_image_set_scale(flash_transform, 1536);
  lv_image_set_rotation(flash_transform, 900);
  lv_obj_set_style_image_colorkey(flash_transform, &flash_key, 0);
  lv_obj_align(flash_transform, LV_ALIGN_TOP_LEFT, 62, 74);

  lv_obj_t *flash_blend = lv_image_create(screen);
  lv_image_set_src(flash_blend, &g_flash_rgb565_image);
  lv_image_set_scale(flash_blend, 1024);
  lv_obj_set_style_image_opa(flash_blend, LV_OPA_50, 0);
  lv_obj_align(flash_blend, LV_ALIGN_TOP_RIGHT, -68, 84);

  lv_obj_t *ram_argb = lv_image_create(screen);
  lv_image_set_src(ram_argb, &g_ram_argb_image);
  lv_image_set_scale_x(ram_argb, 640);
  lv_image_set_scale_y(ram_argb, 384);
  lv_obj_set_style_image_opa(ram_argb, LV_OPA_80, 0);
  lv_obj_align(ram_argb, LV_ALIGN_CENTER, 0, -30);

  lv_obj_t *ram_a8 = lv_image_create(screen);
  lv_image_set_src(ram_a8, &g_ram_a8_image);
  lv_obj_set_style_image_recolor(ram_a8, lv_color_hex(0xffa000), 0);
  lv_obj_set_style_image_recolor_opa(ram_a8, LV_OPA_COVER, 0);
  lv_obj_align(ram_a8, LV_ALIGN_CENTER, 0, 70);

  lv_obj_t *label = lv_label_create(screen);
  lv_label_set_text(label, "LVGL 9.1 + ESP32-P4 PPA");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 18);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  struct lv_draw_ppa_nuttx_stats_s stats;
  bool stats_reported = false;
  int ret;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      fprintf(stderr, "lvgl_ppa: LVGL is already initialized\n");
      return 1;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "lvgl_ppa: display initialization failed\n");
      lv_deinit();
      return 1;
    }

  ret = lv_draw_ppa_nuttx_init();
  if (ret < 0)
    {
      fprintf(stderr, "lvgl_ppa: PPA draw unit initialization failed: %d\n",
              ret);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return 1;
    }

  lvgl_ppa_create_ui();
  while (1)
    {
      uint32_t idle = lv_timer_handler();

      if (!stats_reported && lv_draw_ppa_nuttx_get_stats(&stats) == 0 &&
          (stats.accelerated != 0 || stats.fallback != 0))
        {
          printf("lvgl_ppa: accelerated=%" PRIu32 " fills=%" PRIu32
                 " copies=%" PRIu32 " blends=%" PRIu32
                 " transforms=%" PRIu32 " flash=%" PRIu32
                 " direct=%" PRIu32 " staged=%" PRIu32
                 " fallback=%" PRIu32 "\n",
                 stats.accelerated, stats.fills, stats.copies,
                 stats.blends, stats.transforms, stats.flash_inputs,
                 stats.direct, stats.staged, stats.fallback);
          stats_reported = true;
        }

      usleep((idle == 0 ? 1 : idle) * 1000);
    }

  return 0;
}
