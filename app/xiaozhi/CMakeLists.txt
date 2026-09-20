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
      platform/nuttx/nuttx_bsp_test.cpp
      platform/nuttx/nuttx_identity.cpp
      platform/nuttx/nuttx_wifi.cpp
      platform/nuttx/nuttx_lvgl_display.cpp)

  set(XIAOZHI_UI_SOURCES
      ui/lv_100ask_xz_ai_main.c
      ui/wifi_ui.c
      ui/bsp_test_ui.c
      ui/xiaozhi_font.c
      ui/xiaozhi_misans_gb2312_16.c
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

  # Build the persistent LittleFS image separately from nuttx.bin.  This keeps
  # normal firmware updates small and, more importantly, prevents them from
  # overwriting files created at runtime in /data.  The image/flash targets are
  # intended for initial provisioning or an explicit data-partition reset.

  get_filename_component(OPENVELA_ROOT "${NUTTX_APPS_DIR}/.." ABSOLUTE)
  set(XIAOZHI_DATA_DIR "${CMAKE_BINARY_DIR}/xiaozhi-data")
  set(XIAOZHI_DATA_IMAGE "${CMAKE_BINARY_DIR}/xiaozhi-data.bin")
  set(XIAOZHI_MKLITTLEFS
      "${OPENVELA_ROOT}/vendor/artinchip/tools/scripts/mklittlefs")

  add_custom_command(
    OUTPUT "${XIAOZHI_DATA_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${XIAOZHI_DATA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XIAOZHI_DATA_DIR}"
    COMMAND "${XIAOZHI_MKLITTLEFS}"
            -c "${XIAOZHI_DATA_DIR}"
            -b 4096 -p 256 -s 4194304 "${XIAOZHI_DATA_IMAGE}"
    DEPENDS "${XIAOZHI_MKLITTLEFS}"
    COMMENT "Creating xiaozhi 4 MiB LittleFS data image"
    VERBATIM)

  add_custom_target(xiaozhi_data_image DEPENDS "${XIAOZHI_DATA_IMAGE}")

  if(TARGET nuttx_post_build)
    add_custom_target(
      xiaozhi_partition_check ALL
      COMMAND ${CMAKE_COMMAND}
              -DFIRMWARE_IMAGE=${CMAKE_BINARY_DIR}/nuttx.bin
              -P "${CMAKE_CURRENT_LIST_DIR}/tools/check_partition.cmake"
      DEPENDS nuttx_post_build
      COMMENT "Checking that nuttx.bin does not overlap /data"
      VERBATIM)
  endif()

  add_custom_target(
    xiaozhi_data_flash
    COMMAND ${CMAKE_COMMAND}
            -DXIAOZHI_DATA_IMAGE=${XIAOZHI_DATA_IMAGE}
            -P "${CMAKE_CURRENT_LIST_DIR}/tools/flash_data.cmake"
    DEPENDS xiaozhi_data_image
    COMMENT "Flashing xiaozhi LittleFS data partition at 0xC00000"
    VERBATIM)
endif()
