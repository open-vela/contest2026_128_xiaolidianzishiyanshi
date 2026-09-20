# XiaoZhi AI client for openvela/NuttX.

if(CONFIG_LVX_USE_DEMO_CONTEST2026_128_XIAOZHI)
  set(XIAOZHI_SOURCES
      src/main.cpp
      src/application.cpp
      src/activation.cpp
      src/protocol.cpp
      src/device_state_machine.cpp
      platform/nuttx/lws_websocket.cpp
      platform/nuttx/nuttx_http.cpp
      platform/nuttx/nuttx_audio.cpp
      platform/nuttx/nuttx_identity.cpp
      platform/nuttx/nuttx_wifi.cpp
      platform/nuttx/nuttx_lvgl_display.cpp)

  set(XIAOZHI_UI_SOURCES
      ui/lv_100ask_xz_ai_main.c
      ui/wifi_ui.c
      ui/xiaozhi_font.c
      ui/font_awesome_20_4.c
      ui/font_awesome_30_4.c)

  set(XIAOZHI_PPA_SOURCES)
  if(CONFIG_CONTEST2026_128_XIAOZHI_PPA)
    list(APPEND XIAOZHI_PPA_SOURCES
      ${NUTTX_APPS_DIR}/packages/demos/contest2026_128_hello_app/lv_draw_ppa_nuttx.c)
  endif()

  nuttx_add_application(
    NAME ${CONFIG_CONTEST2026_128_XIAOZHI_PROGNAME}
    SRCS ${XIAOZHI_SOURCES} ${XIAOZHI_UI_SOURCES} ${XIAOZHI_PPA_SOURCES}
    STACKSIZE ${CONFIG_CONTEST2026_128_XIAOZHI_STACKSIZE}
    PRIORITY ${CONFIG_CONTEST2026_128_XIAOZHI_PRIORITY}
    INCLUDE_DIRECTORIES
      ${CMAKE_CURRENT_LIST_DIR}/include
      ${CMAKE_CURRENT_LIST_DIR}/ui
      ${CMAKE_CURRENT_LIST_DIR}/platform/nuttx
      ${NUTTX_APPS_DIR}/packages/demos/contest2026_128_hello_app
      ${NUTTX_CHIP_ABS_DIR}
      ${NUTTX_APPS_DIR}/netutils/libwebsockets
      ${NUTTX_APPS_DIR}/netutils/libwebsockets/libwebsockets/include
      ${NUTTX_APPS_DIR}/external/opus/opus/include
    DEPENDS lvgl curl)

  set_source_files_properties(${XIAOZHI_SOURCES}
    PROPERTIES COMPILE_FLAGS "-std=gnu++17")
endif()
