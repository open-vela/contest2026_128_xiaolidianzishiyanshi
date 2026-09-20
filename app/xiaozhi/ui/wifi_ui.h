#pragma once

#include <stddef.h>

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_UI_MAX_NETWORKS 24
#define WIFI_UI_SSID_MAX 32

typedef struct {
  char ssid[WIFI_UI_SSID_MAX + 1];
  int rssi;
  int secure;
} wifi_ui_network_t;

typedef enum {
  WIFI_UI_CONNECTION_IDLE = 0,
  WIFI_UI_CONNECTION_AUTHENTICATING,
  WIFI_UI_CONNECTION_DHCP,
  WIFI_UI_CONNECTION_CONNECTED,
  WIFI_UI_CONNECTION_FAILED,
  WIFI_UI_CONNECTION_CANCELED,
} wifi_ui_connection_stage_t;

typedef void (*wifi_ui_scan_callback_t)(void *user_data);
typedef void (*wifi_ui_connect_callback_t)(const wifi_ui_network_t *network,
                                           const char *password,
                                           void *user_data);
typedef void (*wifi_ui_cancel_callback_t)(void *user_data);

typedef struct wifi_ui wifi_ui_t;

wifi_ui_t *wifi_ui_create(lv_obj_t *parent);
void wifi_ui_destroy(wifi_ui_t *ui);
void wifi_ui_set_callbacks(wifi_ui_t *ui,
                           wifi_ui_scan_callback_t scan_callback,
                           wifi_ui_connect_callback_t connect_callback,
                           wifi_ui_cancel_callback_t cancel_callback,
                           void *user_data);
void wifi_ui_set_status(wifi_ui_t *ui, const char *ssid,
                        const char *ip_address, int connected);
void wifi_ui_set_scan_result(wifi_ui_t *ui, int scanning,
                             const wifi_ui_network_t *networks, size_t count,
                             const char *error);
void wifi_ui_set_connection(wifi_ui_t *ui,
                            wifi_ui_connection_stage_t stage,
                            const char *ssid, const char *message);

#ifdef __cplusplus
}
#endif
