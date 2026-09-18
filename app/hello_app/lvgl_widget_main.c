/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/lvgl_widget_main.c
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
#include <stdio.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <lvgl/demos/lv_demos.h>

#include "lv_draw_ppa_nuttx.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  struct lv_draw_ppa_nuttx_stats_s stats;
  int stats_reported = 0;
  int ret;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      fprintf(stderr, "lvgl_widget: LVGL is already initialized\n");
      return 1;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = "/dev/input0";
#endif

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "lvgl_widget: display initialization failed\n");
      lv_deinit();
      return 1;
    }

  ret = lv_draw_ppa_nuttx_init();
  if (ret < 0)
    {
      fprintf(stderr,
              "lvgl_widget: PPA draw unit initialization failed: %d\n",
              ret);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return 1;
    }

  lv_demo_widgets();

  while (1)
    {
      uint32_t idle = lv_timer_handler();
      stats_reported++;
      if (stats_reported >= 100 && lv_draw_ppa_nuttx_get_stats(&stats) == 0 &&
          (stats.accelerated != 0 || stats.fallback != 0))
        {
          printf("lvgl_widget: accelerated=%" PRIu32 " fills=%" PRIu32
                 " copies=%" PRIu32 " blends=%" PRIu32
                 " transforms=%" PRIu32 " flash=%" PRIu32
                 " direct=%" PRIu32 " staged=%" PRIu32
                 " fallback=%" PRIu32 "\n",
                 stats.accelerated, stats.fills, stats.copies,
                 stats.blends, stats.transforms, stats.flash_inputs,
                 stats.direct, stats.staged, stats.fallback);
          stats_reported = 0;
        }

      usleep((idle == 0 ? 1 : idle) * 1000);
    }

  return 0;
}
