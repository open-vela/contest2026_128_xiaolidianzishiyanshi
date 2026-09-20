#pragma once

#include <stddef.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BSP_TEST_MICROPHONE = 0,
  BSP_TEST_SPEAKER,
  BSP_TEST_CAMERA,
  BSP_TEST_PING,
  BSP_TEST_DNS,
} bsp_test_type_t;

typedef void (*bsp_test_callback_t)(bsp_test_type_t type, void *user_data);

typedef struct bsp_test_ui bsp_test_ui_t;

bsp_test_ui_t *bsp_test_ui_create(lv_obj_t *parent);
void bsp_test_ui_destroy(bsp_test_ui_t *ui);
void bsp_test_ui_set_callback(bsp_test_ui_t *ui,
                              bsp_test_callback_t callback,
                              void *user_data);
void bsp_test_ui_set_running(bsp_test_ui_t *ui, bsp_test_type_t type);
void bsp_test_ui_set_result(bsp_test_ui_t *ui, bsp_test_type_t type,
                            int success, const char *message,
                            const uint16_t *pixels, int width, int height);

#ifdef __cplusplus
}
#endif
