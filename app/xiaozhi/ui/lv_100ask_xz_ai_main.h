/**
 * @file lv_modbus_tool.h
 * This file exists only to be compatible with Arduino's library structure
 */

#ifndef LV_100ASK_XZ_AI_MAIN_H
#define LV_100ASK_XZ_AI_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include <lvgl/lvgl.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef void (*lv_100ask_xz_ai_talk_callback_t)(void *user_data);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/
void lv_100ask_xz_ai_main(void);
void lv_100ask_xz_ai_deinit(void);

void SetStateString(const char *str);

void SetWifi(int enabled);

/* 每次只能显示给定的str */
void SetText(const char *str);

void SetEmotion(const char *name);

void SetTalkButtonCallback(lv_100ask_xz_ai_talk_callback_t callback,
                           void *user_data);
void SetTalkButtonState(int listening, int enabled);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*LV_100ASK_XZ_AI_MAIN_H*/
