/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/lvglbench_ppa_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
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
  int ret;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      fprintf(stderr, "lvglbench_ppa: LVGL is already initialized\n");
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
      fprintf(stderr, "lvglbench_ppa: display initialization failed\n");
      lv_deinit();
      return 1;
    }

  ret = lv_draw_ppa_nuttx_init();
  if (ret < 0)
    {
      fprintf(stderr,
              "lvglbench_ppa: PPA draw unit initialization failed: %d\n",
              ret);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return 1;
    }

  lv_demo_benchmark();

  while (1)
    {
      uint32_t idle = lv_timer_handler();
      usleep((idle == 0 ? 1 : idle) * 1000);
    }

  return 0;
}
