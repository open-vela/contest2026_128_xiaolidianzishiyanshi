/**
 ******************************************************************************
 * @file    lv_100ask_xz_ai_main.c
 * @author  百问科技
 * @version V1.0
 * @date    2025-3-17
 * @brief	100ask XiaoZhi AI base on LVGL
 ******************************************************************************
 * Change Logs:
 * Date           Author          Notes
 * 2025-3-17     zhouyuebiao     First version
 * 2025-11-7     zhouyuebiao
 ******************************************************************************
 * @attention
 *
 * Copyright (C) 2008-2025 深圳百问网科技有限公司<https://www.100ask.net/>
 * All rights reserved
 *
 * 代码配套的视频教程：
 *      B站：   https://www.bilibili.com/video/BV1WE421K75k
 *      百问网：https://fnwcn.xetslk.com/s/39njGj
 *      淘宝：  https://detail.tmall.com/item.htm?id=779667445604
 *
 * 本程序遵循MIT协议, 请遵循协议！
 * 免责声明: 百问网编写的文档, 仅供学员学习使用,
 *可以转发或引用(请保留作者信息),禁止用于商业用途！ 免责声明: 百问网编写的程序,
 *仅供学习参考，假如被用于商业用途, 但百问网不承担任何后果！
 *
 * 百问网学习平台   : https://www.100ask.net
 * 百问网交流社区   : https://forums.100ask.net
 * 百问网LVGL文档   : https://lvgl.100ask.net
 * 百问网官方B站    : https://space.bilibili.com/275908810
 * 百问网官方淘宝   : https://100ask.taobao.com
 * 百问网微信公众号 ：百问科技 或 baiwenkeji
 * 联系我们(E-mail):  support@100ask.net 或 fae_100ask@163.com
 *
 *                             版权所有，盗版必究。
 ******************************************************************************
 */

/*********************
 *      INCLUDES
 *********************/
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lv_100ask_xz_ai_main.h"
#include "xiaozhi_font.h"

static pthread_mutex_t lvgl_mutex;

/*********************
 *      DEFINES
 *********************/
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);
// https://gitee.com/weidongshan/lv_100ask_linux_desktop/blob/stm32mp157/v8/lv_100ask_app/src/stm32mp157_app/stm32mp157_set_wlan/src/set_wlan.c
/**********************
 *      TYPEDEFS
 **********************/
typedef struct _lv_100ask_xz_ai {
  lv_obj_t *state_bar_img_wifi;
  lv_obj_t *state_bar_label_state;
  lv_obj_t *state_bar_img_battery;
  lv_obj_t *img_emoji;
  lv_obj_t *label_chat;
  lv_obj_t *button_talk;
  lv_obj_t *label_talk;
} T_lv_100ask_xz_ai, *PT_lv_100ask_xz_ai;

// 符号结构体
typedef struct {
  const char *name;
  const char *utf8_string;
} font_awesome_symbol_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void init_style(void);
static const char *font_awesome_get_utf8(const char *name);
static void talk_button_event_cb(lv_event_t *event);

/**********************
 *  STATIC VARIABLES
 **********************/
static PT_lv_100ask_xz_ai g_pt_lv_100ask_xz_ai;

static lv_style_t g_style_chat_font;
static lv_style_t g_style_state_font;

static lv_100ask_xz_ai_talk_callback_t g_talk_callback;
static void *g_talk_callback_user_data;

static const font_awesome_symbol_t font_awesome_symbols[] = {
    {"neutral", "\xef\x96\xa4"},     {"happy", "\xef\x84\x98"},
    {"laughing", "\xef\x96\x9b"},    {"funny", "\xef\x96\x88"},
    {"sad", "\xee\x8e\x84"},         {"angry", "\xef\x95\x96"},
    {"crying", "\xef\x96\xb3"},      {"loving", "\xef\x96\x84"},
    {"embarrassed", "\xef\x95\xb9"}, {"surprised", "\xee\x8d\xab"},
    {"shocked", "\xee\x8d\xb5"},     {"thinking", "\xee\x8e\x9b"},
    {"winking", "\xef\x93\x9a"},     {"cool", "\xee\x8e\x98"},
    {"relaxed", "\xee\x8e\x92"},     {"delicious", "\xee\x8d\xb2"},
    {"kissy", "\xef\x96\x98"},       {"confident", "\xee\x90\x89"},
    {"sleepy", "\xee\x8e\x8d"},      {"silly", "\xee\x8e\xa4"},
    {"confused", "\xee\x8d\xad"},
};

static const size_t font_awesome_symbol_count =
    sizeof(font_awesome_symbols) / sizeof(font_awesome_symbols[0]);
/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_100ask_xz_ai_main(void) {
  pthread_mutex_init(&lvgl_mutex, NULL);

  /* init */
  g_pt_lv_100ask_xz_ai =
      (T_lv_100ask_xz_ai *)lv_malloc(sizeof(T_lv_100ask_xz_ai));

  init_style();

  /* state bar */
  lv_obj_t *cont_state_bar = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(cont_state_bar);
  lv_obj_set_size(cont_state_bar, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_align(cont_state_bar, LV_ALIGN_TOP_MID);
  lv_obj_set_style_radius(cont_state_bar, 0, 0);
  lv_obj_set_style_bg_opa(cont_state_bar, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(cont_state_bar, 10, 0);
  lv_obj_set_layout(cont_state_bar, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(cont_state_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cont_state_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // wifi
  // {"wifi",         "\xef\x87\xab"},
  // {"wifi_fair",    "\xef\x9a\xab"},
  // {"wifi_weak",    "\xef\x9a\xaa"},
  // {"wifi_slash",   "\xef\x9a\xac"},
  g_pt_lv_100ask_xz_ai->state_bar_img_wifi =
      lv_label_create(cont_state_bar); // lv_image_create(cont_state_bar);
  lv_obj_set_style_text_font(g_pt_lv_100ask_xz_ai->state_bar_img_wifi,
                             &font_awesome_20_4, 0);
  SetWifi(0);

  // state
  g_pt_lv_100ask_xz_ai->state_bar_label_state = lv_label_create(cont_state_bar);
  lv_obj_add_style(g_pt_lv_100ask_xz_ai->state_bar_label_state,
                   &g_style_state_font, 0);
  lv_obj_set_width(g_pt_lv_100ask_xz_ai->state_bar_label_state, LV_PCT(70));
  lv_label_set_text(g_pt_lv_100ask_xz_ai->state_bar_label_state, "Starting");

  // battery
  //{"battery_full", "\xef\x89\x80"},
  //{"battery_three_quarters", "\xef\x89\x81"},
  //{"battery_half", "\xef\x89\x82"},
  //{"battery_quarter", "\xef\x89\x83"},
  //{"battery_empty", "\xef\x89\x84"},
  //{"battery_slash", "\xef\x8d\xb7"},
  //{"battery_bolt", "\xef\x8d\xb6"},
  g_pt_lv_100ask_xz_ai->state_bar_img_battery = lv_label_create(cont_state_bar);
  lv_obj_set_style_text_font(g_pt_lv_100ask_xz_ai->state_bar_img_battery,
                             &font_awesome_20_4, 0);
  lv_label_set_text(g_pt_lv_100ask_xz_ai->state_bar_img_battery,
                    "\xef\x89\x80");

  /* Keep the assistant in the middle third.  WiFi and offline BSP tests own
   * the left and right thirds respectively.
   */
  lv_obj_t *main_content = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(main_content);
  lv_obj_set_size(main_content, LV_PCT(32), LV_PCT(88));
  lv_obj_align(main_content, LV_ALIGN_BOTTOM_MID, 0, -4);

  /* emoji */
  // https://www.iconfont.cn/search/index?searchType=icon&q=%E5%9C%86%E8%84%B8%E8%A1%A8%E6%83%85
  g_pt_lv_100ask_xz_ai->img_emoji = lv_label_create(main_content);
  lv_obj_set_style_text_font(g_pt_lv_100ask_xz_ai->img_emoji,
                             &font_awesome_30_4, 0);
  lv_obj_align(g_pt_lv_100ask_xz_ai->img_emoji, LV_ALIGN_CENTER, 0, -40);

  /* chat */
  g_pt_lv_100ask_xz_ai->label_chat = lv_label_create(main_content);
  lv_obj_set_width(g_pt_lv_100ask_xz_ai->label_chat, LV_PCT(86));
  lv_obj_add_style(g_pt_lv_100ask_xz_ai->label_chat, &g_style_chat_font, 0);
  lv_label_set_text(g_pt_lv_100ask_xz_ai->label_chat,
                    "Connecting to server...");
  lv_obj_align_to(g_pt_lv_100ask_xz_ai->label_chat,
                  g_pt_lv_100ask_xz_ai->img_emoji, LV_ALIGN_OUT_BOTTOM_MID, 0,
                  10);

  /* Manual listening replaces VAD: click once to send listen/start and
   * click again to send listen/stop.
   */
  g_pt_lv_100ask_xz_ai->button_talk = lv_button_create(main_content);
  lv_obj_set_size(g_pt_lv_100ask_xz_ai->button_talk, 160, 48);
  lv_obj_align(g_pt_lv_100ask_xz_ai->button_talk, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_add_event_cb(g_pt_lv_100ask_xz_ai->button_talk, talk_button_event_cb,
                      LV_EVENT_CLICKED, NULL);

  g_pt_lv_100ask_xz_ai->label_talk =
      lv_label_create(g_pt_lv_100ask_xz_ai->button_talk);
  lv_obj_set_style_text_font(g_pt_lv_100ask_xz_ai->label_talk,
                             xiaozhi_font_16(),
                             0);
  lv_label_set_text(g_pt_lv_100ask_xz_ai->label_talk,
                    "开始说话");
  lv_obj_center(g_pt_lv_100ask_xz_ai->label_talk);
  SetTalkButtonState(0, 0);

  SetEmotion("neutral");
}

void lvgl_lock(void) { pthread_mutex_lock(&lvgl_mutex); }

void lvgl_unlock(void) { pthread_mutex_unlock(&lvgl_mutex); }

void SetWifi(int enabled) {
  // https://github.com/78/xiaozhi-fonts/blob/main/src/font_awesome.c
  lvgl_lock();
  if (enabled) {
    // lv_image_set_src(g_pt_lv_100ask_xz_ai->state_bar_img_wifi,
    // "\xef\x87\xab");  // wifi full
    lv_label_set_text(g_pt_lv_100ask_xz_ai->state_bar_img_wifi,
                      "\xef\x87\xab"); // wifi full
  } else {
    // lv_image_set_src(g_pt_lv_100ask_xz_ai->state_bar_img_wifi,
    // "\xef\x9a\xac");  // wifi_slash
    lv_label_set_text(g_pt_lv_100ask_xz_ai->state_bar_img_wifi,
                      "\xef\x9a\xac"); // wifi_slash
  }
  lvgl_unlock();
}

void SetStateString(const char *str) {
  lvgl_lock();
  lv_label_set_text(g_pt_lv_100ask_xz_ai->state_bar_label_state, str);
  lvgl_unlock();
}

void SetText(const char *str) {
  lvgl_lock();
  lv_label_set_text(g_pt_lv_100ask_xz_ai->label_chat, str);
  lvgl_unlock();
}

void SetEmotion(const char *name) {
  const char *symbol = font_awesome_get_utf8(name);
  if (symbol == NULL) {
    symbol = font_awesome_get_utf8("neutral");
  }
  lvgl_lock();
  lv_label_set_text(g_pt_lv_100ask_xz_ai->img_emoji, symbol);
  lvgl_unlock();
}

void SetTalkButtonCallback(lv_100ask_xz_ai_talk_callback_t callback,
                           void *user_data) {
  g_talk_callback = callback;
  g_talk_callback_user_data = user_data;
}

void SetTalkButtonState(int listening, int enabled) {
  if (g_pt_lv_100ask_xz_ai == NULL ||
      g_pt_lv_100ask_xz_ai->button_talk == NULL) {
    return;
  }

  lvgl_lock();
  lv_label_set_text(g_pt_lv_100ask_xz_ai->label_talk,
                    listening ? "停止说话" : "开始说话");
  if (enabled) {
    lv_obj_remove_state(g_pt_lv_100ask_xz_ai->button_talk, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(g_pt_lv_100ask_xz_ai->button_talk, LV_STATE_DISABLED);
  }
  lv_obj_set_style_bg_color(g_pt_lv_100ask_xz_ai->button_talk,
                            listening ? lv_color_hex(0xd63b3b)
                                      : lv_color_hex(0x2f80ed),
                            LV_PART_MAIN);
  lvgl_unlock();
}

/**********************
 *   STATIC FUNCTIONS
 ***********************/

static const char *font_awesome_get_utf8(const char *name) {
  if (!name)
    return NULL;

  for (size_t i = 0; i < font_awesome_symbol_count; i++) {
    if (strcmp(font_awesome_symbols[i].name, name) == 0) {
      return font_awesome_symbols[i].utf8_string;
    }
  }
  return NULL;
}

void lv_100ask_xz_ai_deinit(void) {
  if (!g_pt_lv_100ask_xz_ai) {
    return;
  }

  lv_obj_clean(lv_screen_active());
  lv_free(g_pt_lv_100ask_xz_ai);
  g_pt_lv_100ask_xz_ai = NULL;
  g_talk_callback = NULL;
  g_talk_callback_user_data = NULL;
  pthread_mutex_destroy(&lvgl_mutex);
}

static void talk_button_event_cb(lv_event_t *event) {
  (void)event;
  if (g_talk_callback != NULL) {
    g_talk_callback(g_talk_callback_user_data);
  }
}

static void init_style(void) {
  /*Create style with the new font*/;
  lv_style_init(&g_style_chat_font);
  lv_style_set_text_font(&g_style_chat_font, xiaozhi_font_16());
  lv_style_set_text_align(&g_style_chat_font, LV_TEXT_ALIGN_CENTER);

  lv_style_init(&g_style_state_font);
  lv_style_set_text_font(&g_style_state_font, xiaozhi_font_16());
  lv_style_set_text_align(&g_style_state_font, LV_TEXT_ALIGN_CENTER);
}
