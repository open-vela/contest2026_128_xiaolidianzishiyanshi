#include "xiaozhi_font.h"

#include <stdbool.h>

static lv_font_t g_xiaozhi_font_16;
static bool g_xiaozhi_font_initialized;

const lv_font_t *xiaozhi_font_16(void) {
  if (!g_xiaozhi_font_initialized) {
    g_xiaozhi_font_16 = *LV_FONT_DEFAULT;
#if LV_FONT_SIMSUN_16_CJK
    g_xiaozhi_font_16.fallback = &lv_font_simsun_16_cjk;
#endif
    g_xiaozhi_font_initialized = true;
  }
  return &g_xiaozhi_font_16;
}
