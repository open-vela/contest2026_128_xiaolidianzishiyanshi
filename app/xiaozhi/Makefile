include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_CONTEST2026_128_XIAOZHI_PROGNAME)
PRIORITY  = $(CONFIG_CONTEST2026_128_XIAOZHI_PRIORITY)
STACKSIZE = $(CONFIG_CONTEST2026_128_XIAOZHI_STACKSIZE)
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_128_XIAOZHI)

CXXSRCS = src/main.cpp src/application.cpp src/protocol.cpp
CXXSRCS += src/device_state_machine.cpp platform/nuttx/lws_websocket.cpp
CXXSRCS += platform/nuttx/nuttx_audio.cpp platform/nuttx/nuttx_identity.cpp
CXXSRCS += platform/nuttx/nuttx_lvgl_display.cpp

CSRCS = ui/lv_100ask_xz_ai_main.c ui/font_awesome_20_4.c
CSRCS += ui/font_awesome_30_4.c

CXXFLAGS += -std=gnu++17
CXXFLAGS += -I$(APPDIR)/packages/demos/contest2026_128_xiaozhi/include
CXXFLAGS += -I$(APPDIR)/packages/demos/contest2026_128_xiaozhi/ui
CXXFLAGS += -I$(APPDIR)/packages/demos/contest2026_128_xiaozhi/platform/nuttx
CXXFLAGS += -I$(APPDIR)/netutils/libwebsockets/libwebsockets/include
CXXFLAGS += -I$(APPDIR)/external/opus/opus/include
CFLAGS += -I$(APPDIR)/packages/demos/contest2026_128_xiaozhi/ui

include $(APPDIR)/Application.mk
