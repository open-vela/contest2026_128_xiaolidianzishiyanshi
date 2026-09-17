/****************************************************************************
 * apps/packages/demos/contest2026_128_hello_app/lv_draw_ppa_nuttx.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_PACKAGES_DEMOS_CONTEST2026_128_HELLO_APP_LV_DRAW_PPA_NUTTX_H
#define __APPS_PACKAGES_DEMOS_CONTEST2026_128_HELLO_APP_LV_DRAW_PPA_NUTTX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct lv_draw_ppa_nuttx_stats_s
{
  uint32_t accelerated;
  uint32_t fills;
  uint32_t copies;
  uint32_t blends;
  uint32_t transforms;
  uint32_t flash_inputs;
  uint32_t direct;
  uint32_t staged;
  uint32_t fallback;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Call after lv_init(), and before creating or rendering the first UI. */

int lv_draw_ppa_nuttx_init(void);
int lv_draw_ppa_nuttx_get_stats(struct lv_draw_ppa_nuttx_stats_s *stats);

#endif
