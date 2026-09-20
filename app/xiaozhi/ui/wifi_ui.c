#include "wifi_ui.h"
#include "src/font/lv_font.h"
#include "src/font/lv_symbol_def.h"
#include "xiaozhi_font.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(font_awesome_20_4);

#define WIFI_ICON "\xef\x87\xab"
#define SEARCH_ICON "\xef\x80\x82"
#define LOCK_ICON "\xef\x80\xa3"

struct wifi_ui {
  lv_obj_t *panel;
  lv_obj_t *ssid_label;
  lv_obj_t *ip_label;
  lv_obj_t *scan_button;
  lv_obj_t *list;
  lv_obj_t *dialog_overlay;
  lv_obj_t *dialog_card;
  lv_obj_t *dialog_title;
  lv_obj_t *password_textarea;
  lv_obj_t *keyboard;
  lv_obj_t *dialog_actions;
  lv_obj_t *progress_bar;
  lv_obj_t *progress_label;
  lv_obj_t *cancel_button;
  wifi_ui_network_t networks[WIFI_UI_MAX_NETWORKS];
  size_t network_count;
  size_t selected_index;
  wifi_ui_scan_callback_t scan_callback;
  wifi_ui_connect_callback_t connect_callback;
  wifi_ui_cancel_callback_t cancel_callback;
  void *callback_user_data;
};

static void set_text_font(lv_obj_t *object) {
  lv_obj_set_style_text_font(object, xiaozhi_font_16(), LV_PART_MAIN);
}

static void close_dialog(wifi_ui_t *ui) {
  if (ui->dialog_overlay != NULL) {
    lv_obj_delete(ui->dialog_overlay);
  }
  ui->dialog_overlay = NULL;
  ui->dialog_card = NULL;
  ui->dialog_title = NULL;
  ui->password_textarea = NULL;
  ui->keyboard = NULL;
  ui->dialog_actions = NULL;
  ui->progress_bar = NULL;
  ui->progress_label = NULL;
  ui->cancel_button = NULL;
}

static lv_obj_t *create_text_button(lv_obj_t *parent, const char *text,
                                    lv_event_cb_t callback,
                                    void *user_data) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_height(button, 42);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
  lv_obj_t *label = lv_label_create(button);
  set_text_font(label);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

static void cancel_dialog_event(lv_event_t *event) {
  wifi_ui_t *ui = lv_event_get_user_data(event);
  if (ui->cancel_callback != NULL) {
    ui->cancel_callback(ui->callback_user_data);
  }
  close_dialog(ui);
}

static void show_progress_dialog(wifi_ui_t *ui, const char *ssid) {
  if (ui->dialog_overlay == NULL) {
    ui->dialog_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(ui->dialog_overlay);
    lv_obj_set_size(ui->dialog_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->dialog_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->dialog_overlay, LV_OPA_60, 0);
    lv_obj_center(ui->dialog_overlay);

    ui->dialog_card = lv_obj_create(ui->dialog_overlay);
    lv_obj_set_size(ui->dialog_card, LV_PCT(58), 210);
    lv_obj_center(ui->dialog_card);
  } else {
    lv_obj_clean(ui->dialog_card);
  }

  ui->password_textarea = NULL;
  ui->keyboard = NULL;
  ui->dialog_actions = NULL;

  ui->dialog_title = lv_label_create(ui->dialog_card);
  set_text_font(ui->dialog_title);
  lv_label_set_text_fmt(ui->dialog_title, "%s\n%s", ssid, "正在连接");
  lv_obj_align(ui->dialog_title, LV_ALIGN_TOP_MID, 0, 8);

  ui->progress_label = lv_label_create(ui->dialog_card);
  set_text_font(ui->progress_label);
  lv_label_set_text(ui->progress_label, "身份验证");
  lv_obj_align(ui->progress_label, LV_ALIGN_CENTER, 0, -8);

  ui->progress_bar = lv_bar_create(ui->dialog_card);
  lv_obj_set_size(ui->progress_bar, LV_PCT(82), 18);
  lv_obj_align(ui->progress_bar, LV_ALIGN_CENTER, 0, 24);
  lv_bar_set_range(ui->progress_bar, 0, 100);
  lv_bar_set_value(ui->progress_bar, 30, LV_ANIM_ON);

  ui->cancel_button = create_text_button(ui->dialog_card, "取消",
                                         cancel_dialog_event, ui);
  lv_obj_set_width(ui->cancel_button, 110);
  lv_obj_align(ui->cancel_button, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void connect_password_event(lv_event_t *event) {
  wifi_ui_t *ui = lv_event_get_user_data(event);
  const wifi_ui_network_t *network = &ui->networks[ui->selected_index];
  char password[65];
  const char *text = lv_textarea_get_text(ui->password_textarea);
  snprintf(password, sizeof(password), "%s", text == NULL ? "" : text);
  show_progress_dialog(ui, network->ssid);
  if (ui->connect_callback != NULL) {
    ui->connect_callback(network, password, ui->callback_user_data);
  }
}

static void show_password_dialog(wifi_ui_t *ui) {
  const wifi_ui_network_t *network = &ui->networks[ui->selected_index];
  close_dialog(ui);

  ui->dialog_overlay = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(ui->dialog_overlay);
  lv_obj_set_size(ui->dialog_overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(ui->dialog_overlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(ui->dialog_overlay, LV_OPA_60, 0);
  lv_obj_center(ui->dialog_overlay);

  ui->dialog_card = lv_obj_create(ui->dialog_overlay);
  lv_obj_set_size(ui->dialog_card, LV_PCT(72), LV_PCT(90));
  lv_obj_center(ui->dialog_card);

  ui->dialog_title = lv_label_create(ui->dialog_card);
  set_text_font(ui->dialog_title);
  lv_label_set_text_fmt(ui->dialog_title, "%s: %s", "输入密码", network->ssid);
  lv_obj_align(ui->dialog_title, LV_ALIGN_TOP_MID, 0, 2);

  ui->password_textarea = lv_textarea_create(ui->dialog_card);
  set_text_font(ui->password_textarea);
  lv_textarea_set_one_line(ui->password_textarea, true);
  lv_textarea_set_password_mode(ui->password_textarea, true);
  lv_textarea_set_max_length(ui->password_textarea, 63);
  lv_textarea_set_placeholder_text(ui->password_textarea, "WiFi 密码");
  lv_obj_set_size(ui->password_textarea, LV_PCT(90), 42);
  lv_obj_align(ui->password_textarea, LV_ALIGN_TOP_MID, 0, 36);

  ui->dialog_actions = lv_obj_create(ui->dialog_card);
  lv_obj_remove_style_all(ui->dialog_actions);
  lv_obj_set_size(ui->dialog_actions, LV_PCT(90), 46);
  lv_obj_set_layout(ui->dialog_actions, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ui->dialog_actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ui->dialog_actions, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(ui->dialog_actions, LV_ALIGN_TOP_MID, 0, 82);

  lv_obj_t *cancel = create_text_button(ui->dialog_actions, "取消",
                                        cancel_dialog_event, ui);
  lv_obj_set_width(cancel, 120);
  lv_obj_t *connect = create_text_button(ui->dialog_actions, "连接",
                                         connect_password_event, ui);
  lv_obj_set_width(connect, 120);

  ui->keyboard = lv_keyboard_create(ui->dialog_card);
  lv_obj_set_style_text_font(ui->keyboard, xiaozhi_font_16(), LV_PART_ITEMS);
  lv_obj_set_size(ui->keyboard, LV_PCT(100), LV_PCT(52));
  lv_obj_align(ui->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(ui->keyboard, ui->password_textarea);
  lv_obj_add_state(ui->password_textarea, LV_STATE_FOCUSED);
}

static void network_event(lv_event_t *event) {
  wifi_ui_t *ui = lv_event_get_user_data(event);
  lv_obj_t *row = lv_event_get_current_target(event);
  ui->selected_index = (size_t)(uintptr_t)lv_obj_get_user_data(row);
  if (ui->selected_index >= ui->network_count) {
    return;
  }

  if (ui->networks[ui->selected_index].secure) {
    show_password_dialog(ui);
  } else {
    const wifi_ui_network_t *network = &ui->networks[ui->selected_index];
    show_progress_dialog(ui, network->ssid);
    if (ui->connect_callback != NULL) {
      ui->connect_callback(network, "", ui->callback_user_data);
    }
  }
}

static void scan_event(lv_event_t *event) {
  wifi_ui_t *ui = lv_event_get_user_data(event);
  if (ui->scan_callback != NULL) {
    ui->scan_callback(ui->callback_user_data);
  }
}

static void show_empty_state(wifi_ui_t *ui, const char *message) {
  lv_obj_clean(ui->list);
  lv_obj_set_layout(ui->list, LV_LAYOUT_NONE);
  lv_obj_t *icon = lv_label_create(ui->list);
  lv_obj_set_style_text_font(icon, &font_awesome_20_4, 0);
  lv_label_set_text(icon, WIFI_ICON);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -18);
  lv_obj_t *label = lv_label_create(ui->list);
  set_text_font(label);
  lv_label_set_text(label, message);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 18);
}

wifi_ui_t *wifi_ui_create(lv_obj_t *parent) {
  wifi_ui_t *ui = lv_malloc_zeroed(sizeof(wifi_ui_t));
  if (ui == NULL) {
    return NULL;
  }

  ui->panel = lv_obj_create(parent);
  lv_obj_set_size(ui->panel, LV_PCT(32), LV_PCT(88));
  lv_obj_align(ui->panel, LV_ALIGN_BOTTOM_LEFT, 4, -4);
  lv_obj_set_style_pad_all(ui->panel, 8, 0);
  lv_obj_set_style_radius(ui->panel, 12, 0);

  lv_obj_t *info = lv_obj_create(ui->panel);
  lv_obj_remove_style_all(info);
  lv_obj_set_size(info, LV_PCT(100), 88);
  lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *wifi_icon = lv_label_create(info);
  lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_16, 0);
  lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 2, 8);

  ui->ssid_label = lv_label_create(info);
  set_text_font(ui->ssid_label);
  lv_obj_set_width(ui->ssid_label, LV_PCT(68));
  lv_label_set_long_mode(ui->ssid_label, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui->ssid_label, "当前连接: 未连接");
  lv_obj_align(ui->ssid_label, LV_ALIGN_TOP_LEFT, 30, 4);

  ui->ip_label = lv_label_create(info);
  set_text_font(ui->ip_label);
  lv_label_set_text(ui->ip_label, "IP: --");
  lv_obj_align(ui->ip_label, LV_ALIGN_TOP_LEFT, 30, 32);

  ui->scan_button = lv_button_create(info);
  lv_obj_set_size(ui->scan_button, 48, 48);
  lv_obj_align(ui->scan_button, LV_ALIGN_RIGHT_MID, -2, 0);
  lv_obj_add_event_cb(ui->scan_button, scan_event, LV_EVENT_CLICKED, ui);
  lv_obj_t *search_icon = lv_label_create(ui->scan_button);
  lv_obj_set_style_text_font(search_icon, &lv_font_montserrat_16, 0);
  lv_label_set_text(search_icon, LV_SYMBOL_REFRESH);
  lv_obj_center(search_icon);

  ui->list = lv_obj_create(ui->panel);
  lv_obj_set_size(ui->list, LV_PCT(100), LV_PCT(74));
  lv_obj_align(ui->list, LV_ALIGN_BOTTOM_MID, 0, 0);
  show_empty_state(ui, "请先扫描WiFi");
  return ui;
}

void wifi_ui_destroy(wifi_ui_t *ui) {
  if (ui == NULL) {
    return;
  }
  close_dialog(ui);
  if (ui->panel != NULL) {
    lv_obj_delete(ui->panel);
  }
  lv_free(ui);
}

void wifi_ui_set_callbacks(wifi_ui_t *ui,
                           wifi_ui_scan_callback_t scan_callback,
                           wifi_ui_connect_callback_t connect_callback,
                           wifi_ui_cancel_callback_t cancel_callback,
                           void *user_data) {
  ui->scan_callback = scan_callback;
  ui->connect_callback = connect_callback;
  ui->cancel_callback = cancel_callback;
  ui->callback_user_data = user_data;
}

void wifi_ui_set_status(wifi_ui_t *ui, const char *ssid,
                        const char *ip_address, int connected) {
  if (ui == NULL) {
    return;
  }
  lv_label_set_text_fmt(ui->ssid_label, "%s: %s", "当前连接",
                        connected && ssid[0] != '\0' ? ssid : "未连接");
  lv_label_set_text_fmt(ui->ip_label, "IP: %s",
                        connected && ip_address[0] != '\0' ? ip_address : "--");
}

void wifi_ui_set_scan_result(wifi_ui_t *ui, int scanning,
                             const wifi_ui_network_t *networks, size_t count,
                             const char *error) {
  if (ui == NULL) {
    return;
  }
  if (scanning) {
    lv_obj_add_state(ui->scan_button, LV_STATE_DISABLED);
    show_empty_state(ui, "正在扫描");
    return;
  }

  lv_obj_remove_state(ui->scan_button, LV_STATE_DISABLED);
  if (error != NULL && error[0] != '\0') {
    show_empty_state(ui, error);
    return;
  }

  ui->network_count = count > WIFI_UI_MAX_NETWORKS ? WIFI_UI_MAX_NETWORKS : count;
  if (ui->network_count == 0) {
    show_empty_state(ui, "暂无可用WiFi");
    return;
  }

  memcpy(ui->networks, networks,
         ui->network_count * sizeof(wifi_ui_network_t));
  lv_obj_clean(ui->list);
  lv_obj_set_style_pad_top(ui->list, 6, 0);
  lv_obj_set_layout(ui->list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ui->list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui->list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);

  size_t index;
  for (index = 0; index < ui->network_count; ++index) {
    lv_obj_t *row = lv_button_create(ui->list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 46);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, network_event, LV_EVENT_CLICKED, ui);
    lv_obj_set_user_data(row, (void *)(uintptr_t)index);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_t *ssid = lv_label_create(row);
    set_text_font(ssid);
    lv_obj_set_flex_grow(ssid, 1);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_label_set_text(ssid, ui->networks[index].ssid);
  }
}

void wifi_ui_set_connection(wifi_ui_t *ui,
                            wifi_ui_connection_stage_t stage,
                            const char *ssid, const char *message) {
  if (ui == NULL) {
    return;
  }
  if (stage == WIFI_UI_CONNECTION_CONNECTED ||
      stage == WIFI_UI_CONNECTION_CANCELED) {
    close_dialog(ui);
    return;
  }
  if (ui->dialog_overlay == NULL || ui->progress_bar == NULL) {
    show_progress_dialog(ui, ssid);
  }
  if (stage == WIFI_UI_CONNECTION_AUTHENTICATING) {
    lv_label_set_text(ui->progress_label, "身份验证");
    lv_bar_set_value(ui->progress_bar, 30, LV_ANIM_ON);
  } else if (stage == WIFI_UI_CONNECTION_DHCP) {
    lv_label_set_text(ui->progress_label, "DHCP 获取地址");
    lv_bar_set_value(ui->progress_bar, 72, LV_ANIM_ON);
  } else if (stage == WIFI_UI_CONNECTION_FAILED) {
    lv_label_set_text_fmt(ui->progress_label, "%s: %s", "连接失败",
                          message == NULL ? "" : message);
    lv_bar_set_value(ui->progress_bar, 0, LV_ANIM_OFF);
  }
}
