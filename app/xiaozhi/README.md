# XiaoZhi AI for OpenVela/NuttX

This directory contains a native NuttX port of the XiaoZhi voice assistant
client.  It follows the message flow and binary framing of the upstream
[`78/xiaozhi-esp32`](https://github.com/78/xiaozhi-esp32) project while keeping
ESP-IDF and board support out of the business layer.

Upstream reference inspected for this port:

- repository: `https://github.com/78/xiaozhi-esp32`
- commit: `5d54beb743ff49c4e8db81bbef9413bdd6e2ba17`
- license: MIT (copied as `LICENSE`)

## Layout

- `src/application.cpp`: platform-independent conversation flow and MCP
  handshake.
- `src/protocol.cpp`: XiaoZhi JSON messages and WebSocket binary protocol
  versions 1, 2 and 3.
- `src/device_state_machine.cpp`: application state validation.
- `platform/nuttx/lws_websocket.cpp`: TLS WebSocket transport using the
  openvela-packaged libwebsockets client and a POSIX thread.
- `platform/nuttx/nuttx_audio.cpp`: NuttX upper-half Audio API adapter using
  `AUDIOIOC_*`, message queues and Opus.
- `platform/nuttx/nuttx_identity.cpp`: network-interface device ID and a
  persistent client UUID using NuttX socket/file/random APIs.
- `platform/nuttx/nuttx_wifi.cpp`: background WAPI scan/association, cancellable
  DHCP and IPv4-change monitor.  No LVGL header is used by this backend.
- `platform/nuttx/nuttx_lvgl_display.cpp`: LVGL 9.1 display adapter.  A single
  pthread owns LVGL and serializes state, transcript, emotion and WiFi updates;
  worker-facing widget calls are protected by `LvglLockGuard`.
- `ui/wifi_ui.c`: standalone LVGL-only WiFi panel, password keyboard and
  authentication/DHCP progress dialog.  It communicates through C callbacks
  and fixed data structures, so it can be run without the network backend.
- `ui/lv_100ask_xz_ai_main.c`: the 100ask XiaoZhi screen and Font Awesome
  assets adapted to the right two thirds of the screen.

No ESP-IDF or FreeRTOS header is used.  Core code depends only on the C++
standard library and cJSON; all OS and device operations live under
`platform/nuttx/`.

## Implemented scope

- Upstream-compatible activation/version request, on-screen and serial pairing
  code, activation polling, and server-provisioned WebSocket URL/token/version.
- WebSocket hello and authentication headers.
- XiaoZhi protocol v1 raw Opus and v2/v3 framed Opus messages.
- Start/stop listening, abort, wake-word notification primitives.
- Touchscreen talk button using upstream-compatible manual listening: one tap
  starts microphone upload and the next tap sends `listen/stop`.  This is the
  default conversation control and does not depend on local VAD.
- TTS audio playback, STT text, LLM emotion and conversation state handling.
- LVGL 9.1 status bar, connection indicator, transcript and emotion display
  through the standard NuttX LCD or framebuffer adapter and optional
  `/dev/input0` touchscreen adapter.
- Left-side WiFi scan list, encrypted/open network indication, on-screen
  password entry, cancellable authentication and DHCP progress, plus live SSID
  and IPv4 address display.  Network work never runs on the LVGL thread.
- MCP initialize and empty tool-list responses.  Board-specific MCP tools can
  be registered later without changing the transport.
- 16 kHz mono PCM capture to 60 ms Opus frames and server-selected-rate Opus
  playback through standard NuttX Audio upper-half devices.

Firmware image installation, downloadable assets, local wake-word engines,
AEC and the optional MQTT/UDP transport remain outside this port.  The
platform-independent activation/configuration portion of the upstream OTA
client is included because it is required to pair a device and obtain its
authenticated WebSocket settings.

## Integration

The repo manifest maps this directory into:

```text
packages/demos/contest2026_128_xiaozhi
```

The Kconfig symbol is
`CONFIG_LVX_USE_DEMO_CONTEST2026_128_XIAOZHI`.  Device paths, endpoint, token,
client-ID path and protocol version are Kconfig parameters.  The dedicated
`esp32p4-function-ev-board/configs/xiaozhi` defconfig enables the MIPI-DSI
framebuffer (`/dev/fb0`), the GT911 touchscreen (`/dev/input0`), LVGL 9.1,
networking, libc++, Opus, ES8311 and NuttX Audio pieces needed by this
application without changing another build configuration.  Text widgets use
LVGL's default Montserrat 16 font with the built-in SimSun 16 CJK bitmap font
as the Chinese fallback; Font Awesome is used only for status and
WiFi/lock/search pictograms.  PPA acceleration is not enabled.

For bring-up, the configured server and token can be overridden without a
configuration change:

```text
nsh> xiaozhi wss://server.example/xiaozhi/v1/ optional-token
```

The Function EV Board BSP registers the ES8311 lower halves as
`/dev/audio/pcm_in0` and `/dev/audio/pcm0`.  The codec I2C port, address,
frequency and I2S port are selected by the
`ESP32P4_FUNCTION_EV_BOARD_ES8311_*` Kconfig options.  I2C SDA/SCL and I2S
MCLK/BCLK/WS/DIN/DOUT remain configurable with the existing
`ESPRESSIF_I2C<n>_*` and `ESPRESSIF_I2S0_*` options.  The defconfig contains a
provisional pin assignment that can be replaced once the audio daughterboard
wiring is fixed.  If the speaker path has an external power amplifier, enable
`ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_ENABLE` and configure its GPIO and active
level; it is deliberately disabled in the initial defconfig.

ES8311 capture and playback use a shared 16 kHz hardware clock.  Opus streams
advertised by the server at 8, 12, 24 or 48 kHz are decoded directly to 16 kHz
before playback, so changing the server format does not disturb the microphone
clock.

The bring-up defconfig enables
`CONTEST2026_128_XIAOZHI_TLS_ALLOW_INSECURE`, so WSS can be tested before a CA
bundle is installed.  This prints a warning and is not suitable for a released
image.  For production, disable that option and set
`CONTEST2026_128_XIAOZHI_TLS_CA_PATH` to a PEM CA bundle available on the
target filesystem.

## Current-stage verification

Build with:

```text
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/xiaozhi --cmake -j8
```

Board-level validation still requires the final ES8311 pin assignment and a
flash/serial session.  At NSH, confirm that `/dev/fb0`,
`/dev/audio/pcm_in0`, and `/dev/audio/pcm0` exist, and then launch the network
client.
