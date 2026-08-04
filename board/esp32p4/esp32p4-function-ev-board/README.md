# ESP32-P4 Function EV Board

## MIPI-DSI display

The `mipi` configuration targets the Espressif 7-inch 1024x600 EK79007
MIPI-DSI display on the ESP32-P4X Function EV Board. The currently tested SoC
is ESP32-P4NRW32X revision 3.2. It provides one RGB888 framebuffer at
`/dev/fb0`, backed by external PSRAM. Touch and MIPI CSI are outside this
configuration.

Hardware settings:

- two DSI data lanes at 650 Mbit/s per lane;
- 48 MHz requested pixel clock;
- GPIO 27 panel reset and GPIO 26 backlight enable;
- LDO channel 3 at 2.5 V;
- 1024x600 timing: HBP 160, HSYNC 10, HFP 160, VBP 23, VSYNC 1, VFP 12.

Build from the openvela workspace root using the repository's default ESP HAL
third-party source:

```sh
./build.sh \
  contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/mipi/ \
  --cmake -j32
```

After flashing and booting, verify that `/dev/fb0` exists and run:

```sh
fb /dev/fb0
```

The NSH console uses UART0 on GPIO 37 (TX) and GPIO 38 (RX), at 115200 baud.

The current implementation starts the DSI host vertical color-bar generator
to diagnose the panel link independently from display DMA. The framebuffer
device is registered, but framebuffer contents are not yet the active video
source. See `docs/dev/ESP32P4-MIPI-DSI.md` for the current validation results
and remaining work.
