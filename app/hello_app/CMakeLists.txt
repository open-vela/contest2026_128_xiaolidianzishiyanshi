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
