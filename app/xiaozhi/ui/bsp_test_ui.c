#include "bsp_test_ui.h"
#include "xiaozhi_font.h"

#include <string.h>

#define CAMERA_PREVIEW_WIDTH 320
#define CAMERA_PREVIEW_HEIGHT 180

struct bsp_test_ui {
  lv_obj_t *panel;
  lv_obj_t *dialog_overlay;
  lv_obj_t *dialog_card;
  lv_obj_t *result_label;
  lv_obj_t *action_container;
  lv_obj_t *canvas;
  uint16_t *canvas_pixels;
  bsp_test_type_t active_type;
  bsp_test_callback_t callback;
  void *callback_user_data;
};

struct test_button_context {
  bsp_test_ui_t *ui;
  bsp_test_type_t type;
};

static void set_text_font(lv_obj_t *object) {
  lv_obj_set_style_text_font(object, xiaozhi_font_16(), LV_PART_MAIN);
}

static const char *test_title(bsp_test_type_t type) {
  switch (type) {
    case BSP_TEST_MICROPHONE:
      return "麦克风测试";
    case BSP_TEST_SPEAKER:
      return "喇叭测试";
    case BSP_TEST_CAMERA:
      return "摄像头测试";
    case BSP_TEST_PING:
    case BSP_TEST_DNS:
      return "网络测试";
    case BSP_TEST_SD_MOUNT:
    case BSP_TEST_SD_LIST:
      return "SDMMC 测试";
    default:
      return "BSP 测试";
  }
}

static void close_dialog(bsp_test_ui_t *ui) {
  if (ui->dialog_overlay != NULL) {
    lv_obj_delete(ui->dialog_overlay);
  }
  ui->dialog_overlay = NULL;
  ui->dialog_card = NULL;
  ui->result_label = NULL;
  ui->action_container = NULL;
  ui->canvas = NULL;
  if (ui->canvas_pixels != NULL) {
    lv_free(ui->canvas_pixels);
    ui->canvas_pixels = NULL;
  }
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, void *user_data) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_width(button, LV_PCT(100));
  lv_obj_set_height(button, 48);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
  lv_obj_t *label = lv_label_create(button);
  set_text_font(label);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

static void close_event(lv_event_t *event) {
  close_dialog(lv_event_get_user_data(event));
}

static void run_event(lv_event_t *event) {
  struct test_button_context *context = lv_event_get_user_data(event);
  if (context->ui->callback != NULL) {
    context->ui->callback(context->type, context->ui->callback_user_data);
  }
}

static void free_context_event(lv_event_t *event) {
  lv_free(lv_event_get_user_data(event));
}

static lv_obj_t *create_test_action(bsp_test_ui_t *ui, lv_obj_t *parent,
                                    const char *text,
                                    bsp_test_type_t type) {
  struct test_button_context *context =
      lv_malloc(sizeof(struct test_button_context));
  if (context == NULL) {
    return NULL;
  }
  context->ui = ui;
  context->type = type;
  lv_obj_t *button = create_button(parent, text, run_event, context);
  lv_obj_add_event_cb(button, free_context_event, LV_EVENT_DELETE, context);
  return button;
}

static void show_dialog(bsp_test_ui_t *ui, bsp_test_type_t type) {
  close_dialog(ui);
  ui->active_type = type;

  ui->dialog_overlay = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(ui->dialog_overlay);
  lv_obj_set_size(ui->dialog_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(ui->dialog_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui->dialog_overlay, LV_OPA_60, 0);
  lv_obj_center(ui->dialog_overlay);

  ui->dialog_card = lv_obj_create(ui->dialog_overlay);
  lv_obj_set_size(ui->dialog_card,
                  type == BSP_TEST_CAMERA ? 620 : 500,
                  type == BSP_TEST_CAMERA ? 430 :
                  (type == BSP_TEST_SD_MOUNT ||
                   type == BSP_TEST_SD_LIST) ? 430 : 300);
  lv_obj_center(ui->dialog_card);

  lv_obj_t *title = lv_label_create(ui->dialog_card);
  set_text_font(title);
  lv_label_set_text(title, test_title(type));
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

  lv_obj_t *close = lv_button_create(ui->dialog_card);
  lv_obj_set_size(close, 42, 36);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
  lv_obj_add_event_cb(close, close_event, LV_EVENT_CLICKED, ui);
  lv_obj_t *close_label = lv_label_create(close);
  set_text_font(close_label);
  lv_label_set_text(close_label, "关闭");
  lv_obj_center(close_label);

  ui->result_label = lv_label_create(ui->dialog_card);
  set_text_font(ui->result_label);
  lv_obj_set_width(ui->result_label, LV_PCT(90));
  if (type == BSP_TEST_SD_MOUNT || type == BSP_TEST_SD_LIST) {
    lv_obj_set_height(ui->result_label, 270);
  }
  lv_obj_set_style_text_align(ui->result_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(ui->result_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(ui->result_label, "请选择并开始测试");
  lv_obj_align(ui->result_label, LV_ALIGN_TOP_MID, 0, 48);

  ui->action_container = lv_obj_create(ui->dialog_card);
  lv_obj_remove_style_all(ui->action_container);
  lv_obj_set_layout(ui->action_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ui->action_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ui->action_container, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  if (type == BSP_TEST_CAMERA) {
    lv_obj_set_size(ui->action_container, LV_PCT(90), 50);
    lv_obj_align(ui->action_container, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_t *button = create_test_action(ui, ui->action_container,
                                         "拍照并显示", BSP_TEST_CAMERA);
    if (button != NULL) {
      lv_obj_set_width(button, 180);
    }
  } else if (type == BSP_TEST_PING || type == BSP_TEST_DNS) {
    lv_obj_set_size(ui->action_container, LV_PCT(90), 60);
    lv_obj_align(ui->action_container, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t *ping = create_test_action(ui, ui->action_container,
                                       "Ping 223.5.5.5", BSP_TEST_PING);
    lv_obj_t *dns = create_test_action(ui, ui->action_container,
                                      "域名解析", BSP_TEST_DNS);
    if (ping != NULL) {
      lv_obj_set_width(ping, 190);
    }
    if (dns != NULL) {
      lv_obj_set_width(dns, 160);
    }
  } else if (type == BSP_TEST_SD_MOUNT || type == BSP_TEST_SD_LIST) {
    lv_obj_set_size(ui->action_container, LV_PCT(90), 60);
    lv_obj_align(ui->action_container, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t *mount = create_test_action(ui, ui->action_container,
                                        "挂载 SD 卡", BSP_TEST_SD_MOUNT);
    lv_obj_t *list = create_test_action(ui, ui->action_container,
                                       "列出目录", BSP_TEST_SD_LIST);
    if (mount != NULL) {
      lv_obj_set_width(mount, 180);
    }
    if (list != NULL) {
      lv_obj_set_width(list, 160);
    }
  } else {
    lv_obj_set_size(ui->action_container, LV_PCT(90), 60);
    lv_obj_align(ui->action_container, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t *button = create_test_action(
        ui, ui->action_container,
        type == BSP_TEST_MICROPHONE ? "录制 2 秒并回放" : "播放测试音效",
        type);
    if (button != NULL) {
      lv_obj_set_width(button, 220);
    }
  }
}

static void category_event(lv_event_t *event) {
  struct test_button_context *context = lv_event_get_user_data(event);
  show_dialog(context->ui, context->type);
}

static void add_category_button(bsp_test_ui_t *ui, const char *text,
                                bsp_test_type_t type) {
  struct test_button_context *context =
      lv_malloc(sizeof(struct test_button_context));
  if (context == NULL) {
    return;
  }
  context->ui = ui;
  context->type = type;
  lv_obj_t *button = create_button(ui->panel, text, category_event, context);
  lv_obj_add_event_cb(button, free_context_event, LV_EVENT_DELETE, context);
}

bsp_test_ui_t *bsp_test_ui_create(lv_obj_t *parent) {
  bsp_test_ui_t *ui = lv_malloc_zeroed(sizeof(bsp_test_ui_t));
  if (ui == NULL) {
    return NULL;
  }

  ui->panel = lv_obj_create(parent);
  lv_obj_set_size(ui->panel, LV_PCT(32), LV_PCT(88));
  lv_obj_align(ui->panel, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
  lv_obj_set_style_pad_all(ui->panel, 10, 0);
  lv_obj_set_style_pad_row(ui->panel, 10, 0);
  lv_obj_set_style_radius(ui->panel, 12, 0);
  lv_obj_set_layout(ui->panel, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ui->panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui->panel, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(ui->panel, LV_DIR_VER);

  lv_obj_t *title = lv_label_create(ui->panel);
  set_text_font(title);
  lv_label_set_text(title, "离线 BSP 测试");
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

  add_category_button(ui, "麦克风测试", BSP_TEST_MICROPHONE);
  add_category_button(ui, "喇叭测试", BSP_TEST_SPEAKER);
  add_category_button(ui, "摄像头测试", BSP_TEST_CAMERA);
  add_category_button(ui, "网络测试", BSP_TEST_PING);
  add_category_button(ui, "SDMMC 测试", BSP_TEST_SD_MOUNT);
  return ui;
}

void bsp_test_ui_destroy(bsp_test_ui_t *ui) {
  if (ui == NULL) {
    return;
  }
  close_dialog(ui);
  if (ui->panel != NULL) {
    lv_obj_delete(ui->panel);
  }
  lv_free(ui);
}

void bsp_test_ui_set_callback(bsp_test_ui_t *ui,
                              bsp_test_callback_t callback,
                              void *user_data) {
  if (ui == NULL) {
    return;
  }
  ui->callback = callback;
  ui->callback_user_data = user_data;
}

void bsp_test_ui_set_running(bsp_test_ui_t *ui, bsp_test_type_t type) {
  if (ui == NULL || ui->dialog_overlay == NULL) {
    return;
  }
  if (ui->active_type != type &&
      !((ui->active_type == BSP_TEST_PING || ui->active_type == BSP_TEST_DNS) &&
        (type == BSP_TEST_PING || type == BSP_TEST_DNS)) &&
      !((ui->active_type == BSP_TEST_SD_MOUNT ||
         ui->active_type == BSP_TEST_SD_LIST) &&
        (type == BSP_TEST_SD_MOUNT || type == BSP_TEST_SD_LIST))) {
    return;
  }
  lv_label_set_text_fmt(ui->result_label, "%s：正在执行…", test_title(type));
}

void bsp_test_ui_set_result(bsp_test_ui_t *ui, bsp_test_type_t type,
                            int success, const char *message,
                            const uint16_t *pixels, int width, int height) {
  if (ui == NULL || ui->dialog_overlay == NULL) {
    return;
  }
  if (ui->active_type != type &&
      !((ui->active_type == BSP_TEST_PING || ui->active_type == BSP_TEST_DNS) &&
        (type == BSP_TEST_PING || type == BSP_TEST_DNS)) &&
      !((ui->active_type == BSP_TEST_SD_MOUNT ||
         ui->active_type == BSP_TEST_SD_LIST) &&
        (type == BSP_TEST_SD_MOUNT || type == BSP_TEST_SD_LIST))) {
    return;
  }

  lv_label_set_text_fmt(ui->result_label, "%s：%s\n%s",
                        test_title(type), success ? "通过" : "失败",
                        message == NULL ? "" : message);

  if (type == BSP_TEST_CAMERA && success && pixels != NULL &&
      width == CAMERA_PREVIEW_WIDTH && height == CAMERA_PREVIEW_HEIGHT) {
    if (ui->canvas_pixels == NULL) {
      ui->canvas_pixels = lv_malloc((size_t)width * height * sizeof(uint16_t));
    }
    if (ui->canvas_pixels != NULL) {
      memcpy(ui->canvas_pixels, pixels,
             (size_t)width * height * sizeof(uint16_t));
      if (ui->canvas == NULL) {
        ui->canvas = lv_canvas_create(ui->dialog_card);
      }
      lv_canvas_set_buffer(ui->canvas, ui->canvas_pixels, width, height,
                           LV_COLOR_FORMAT_RGB565);
      lv_obj_align(ui->canvas, LV_ALIGN_CENTER, 0, 12);
      lv_obj_invalidate(ui->canvas);
    }
  }
}
