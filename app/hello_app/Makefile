############################################################################
# Contest 2026 team 000 hello app - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME =
MAINSRC  =
PRIORITY =
STACKSIZE =

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP),)
PROGNAME += hello_app
MAINSRC  += hello_app_main.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 2048
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_128_LCD_TEST),)
PROGNAME += lcd_test
MAINSRC  += lcd_test_main.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 2048
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_128_TOUCH_TEST),)
PROGNAME += touch_test
MAINSRC  += touch_test_main.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 2048
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_128_PPA_TEST),)
PROGNAME += ppa_test
MAINSRC  += ppa_test_main.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 4096
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_128_LVGL_PPA),)
PROGNAME += lvgl_ppa
MAINSRC  += lvgl_ppa_demo_main.c
CSRCS    += lv_draw_ppa_nuttx.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 8192
ifneq ($(CONFIG_LV_USE_DEMO_WIDGETS),)
PROGNAME += lvgl_widget
MAINSRC  += lvgl_widget_main.c
PRIORITY += SCHED_PRIORITY_DEFAULT
STACKSIZE += 32768
endif
endif

MODULE    = y

include $(APPDIR)/Application.mk
