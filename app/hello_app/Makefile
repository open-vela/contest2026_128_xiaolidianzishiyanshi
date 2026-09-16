############################################################################
# Contest 2026 team 000 hello app - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME =
MAINSRC  =

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP),)
PROGNAME += hello_app
MAINSRC  += hello_app_main.c
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_128_LCD_TEST),)
PROGNAME += lcd_test
MAINSRC  += lcd_test_main.c
endif

PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 2048
MODULE    = y

include $(APPDIR)/Application.mk
