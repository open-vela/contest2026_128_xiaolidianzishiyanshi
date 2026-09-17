# Contest 2026 team 000 hello app
if(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP)
  nuttx_add_application(
    NAME hello_app
    SRCS hello_app_main.c
    STACKSIZE 2048)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_128_LCD_TEST)
  nuttx_add_application(
    NAME lcd_test
    SRCS lcd_test_main.c
    STACKSIZE 2048)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_128_TOUCH_TEST)
  nuttx_add_application(
    NAME touch_test
    SRCS touch_test_main.c
    STACKSIZE 2048)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_128_PPA_TEST)
  nuttx_add_application(
    NAME ppa_test
    SRCS ppa_test_main.c
    INCLUDE_DIRECTORIES ${NUTTX_CHIP_ABS_DIR}
    STACKSIZE 4096)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_128_LVGL_PPA)
  nuttx_add_application(
    NAME lvgl_ppa
    SRCS lvgl_ppa_demo_main.c lv_draw_ppa_nuttx.c
    INCLUDE_DIRECTORIES ${NUTTX_CHIP_ABS_DIR}
    STACKSIZE 8192)

  if(CONFIG_LV_USE_DEMO_WIDGETS)
    nuttx_add_application(
      NAME lvgl_widget
      SRCS lvgl_widget_main.c lv_draw_ppa_nuttx.c
      INCLUDE_DIRECTORIES ${NUTTX_CHIP_ABS_DIR}
      STACKSIZE 32768)
  endif()
endif()
