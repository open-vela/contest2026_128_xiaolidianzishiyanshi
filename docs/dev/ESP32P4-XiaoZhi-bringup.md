# ESP32-P4 Function EV Board XiaoZhi bring-up

This guide applies to the `xiaozhi` configuration for ESP32-P4 Function EV
Board Rev3.2.  The initial GPIO values are provisional: confirm them against
the actual ES8311/audio-board wiring before treating an audio failure as a
driver defect.

## 1. Build artifacts

From the OpenVela workspace:

```sh
cd /home/lxy/openvela
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/xiaozhi --cmake -j8
```

The relevant outputs are:

```text
/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi/nuttx
/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi/nuttx.bin
```

The 16 MiB SPI flash reserves its last 4 MiB as a writable LittleFS volume:

| Region | Address | Size | Update policy |
| --- | ---: | ---: | --- |
| Firmware | `0x00002000` | variable, must end before `0x00c00000` | normal build/flash |
| `/data` LittleFS | `0x00c00000` | 4 MiB | provision separately |

The filesystem is mounted automatically at `/data`.  As before, board
bring-up formats it only when mounting fails.  A valid filesystem therefore
survives reboot and normal `nuttx.bin` updates.  It holds the XiaoZhi client
identifier and files created by the application.

When invoking CMake directly, first select the OpenVela toolchain:

```sh
export PATH=/home/lxy/openvela/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/:$PATH
cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi --target resetconfig
cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi -j8
```

After every configuration change, check the generated `.config`; do not infer
the active configuration from an older build directory.

Build the initial data image independently (it is deliberately not part of
the default firmware target):

```sh
cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi \
  --target xiaozhi_data_image
```

This produces
`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi/xiaozhi-data.bin`.
The initial image is empty.  MiSans is compiled into `nuttx.bin` as an
uncompressed LVGL bitmap font containing ASCII and all 7445 GB2312 double-byte
characters at 16 px and 2 bpp.  It does not allocate a TTF cache at runtime;
characters outside GB2312 fall back to LVGL's default Montserrat 16 font.

## 2. Initial hardware configuration

The supplied defconfig uses:

| Function | Initial value |
| --- | --- |
| ES8311 I2C | I2C0, address `0x18`, 400 kHz |
| I2C pins | SDA GPIO7, SCL GPIO8 |
| ES8311 I2S | I2S0, controller/master, 16 kHz |
| I2S pins | DOUT GPIO9, WS GPIO10, DIN GPIO11, BCLK GPIO12, MCLK GPIO13 |
| Capture node | `/dev/audio/pcm_in0` |
| Playback node | `/dev/audio/pcm0` |
| Display node | `/dev/fb0` |

The codec bus, address and frequency are
`ESP32P4_FUNCTION_EV_BOARD_ES8311_*` options.  Pin routing uses the existing
`ESPRESSIF_I2C0_*` and `ESPRESSIF_I2S0_*` options.  If the loudspeaker has an
external amplifier, enable
`ESP32P4_FUNCTION_EV_BOARD_ES8311_PA_ENABLE`, then set its GPIO and active
level.  PA control is initially disabled because its wiring is not known.

ESP32-P4 Rev3.2 uses `CONFIG_ESP32P4_REV_MIN_301`; this is the newest matching
revision choice currently exposed by the chip Kconfig.

## 3. Flash and first boot

Use the binary from the same build as the ELF used for debugging:

```sh
openocd -f board/esp32p4-builtin.cfg \
  -c "init; reset halt; program_esp /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi/nuttx.bin 0x2000 verify; reset run; shutdown"
```

Provision the data partition once, or use the same command when an explicit
factory reset of `/data` is wanted:

```sh
ESPTOOL_PORT=/dev/ttyACM0 \
cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi \
  --target xiaozhi_data_flash
```

`ESPTOOL_BAUD` can override the default 921600 baud.  This target replaces
the complete `/data` volume, so it also deletes runtime-created persistent
files.  Do not run it for routine firmware updates.  Likewise, avoid a
whole-chip erase or a padded/merged full-flash image when `/data` must be
kept; the `program_esp ... nuttx.bin 0x2000` command above does not touch the
partition.

After first boot, verify the persistent mount:

```text
nsh> mount
nsh> ls -l /data
```

At NSH, first inspect the device nodes:

```text
nsh> ls /dev
nsh> ls /dev/audio
```

The audio directory must contain `pcm_in0` and `pcm0`, and `/dev/fb0` must
exist.  Missing audio nodes mean board bring-up failed before the application
started; inspect the `ES8311`/`I2C`/`I2S` boot messages first.

## 4. Verify the codec control bus

With `xiaozhi` stopped, scan the non-reserved range of I2C0:

```text
nsh> i2c dev -b 0 08 77
```

Address `18` should answer.  If it does not:

1. Check SDA/SCL selection, pull-ups and the codec power/reset rails.
2. Confirm whether the board straps the address to `0x18` or another value.
3. Lower the configured bus frequency to 100 kHz to exclude signal-quality
   problems.
4. Verify MCLK separately; an I2C acknowledgement alone does not prove the
   audio clock or data wiring is correct.

## 5. Verify microphone capture

The current diagnostic intentionally does not open the playback device, so it
can run without a loudspeaker attached:

```text
nsh> xiaozhi --audio-loopback 10
```

For compatibility the command is still named `--audio-loopback`, but this
bring-up mode opens only `/dev/audio/pcm_in0`.  It captures 16 kHz mono S16 PCM
and prints one interval per second:

```text
xiaozhi audio: second=1 samples=16320 min=-7210 max=6984 avg=-12
```

After ten intervals it prints `stopping capture`, closes the Audio endpoint,
then prints `capture stopped` and returns to NSH.  If only the first stop line
appears, the hang is in the audio-driver stop/release path rather than the
application timer.  If no interval appears, the capture driver is not
returning dequeue messages.

Use the failure point to narrow the problem:

- Missing/open failure: board registration or device path.
- Configure/start failure: codec control, I2S clocking or Audio upper-half.
- The test runs but captures silence: DIN, microphone bias/input selection,
  gain, or codec analog routing.
- Implausible constant/full-scale min/max: WS/BCLK polarity, slot selection or
  sample-format mismatch.
- Periodic underruns: inspect I2S DMA and scheduling.  The configured ES8311
  buffer is 1920 bytes, exactly one 60 ms mono/S16/16 kHz frame.

The board layer contains a narrow compatibility shim for the current NuttX
ES8311 lower-half: that driver programs a valid 16 kHz format but mistakenly
returns `-ERANGE`, which otherwise prevents the Audio upper-half from entering
the prepared state.  The shim accepts only mono/S16/16 kHz; any other format
error is preserved.  A log line saying `accepting valid 16-kHz PCM
configuration` is therefore expected during this bring-up.  The same board
file also corrects the lower-half's erroneous 12-byte register read to one
byte through an ES8311-only I2C proxy; normal `/dev/i2c0` transfers are not
affected.

A logic analyzer should show continuous MCLK and, while streaming, BCLK and WS
at the expected ratio.  Probe DIN first; playback is deliberately outside this
test and can be enabled after a loudspeaker is attached.

The bring-up defconfig enables one-second microphone statistics while the
application is listening, for example:

```text
xiaozhi audio: samples=16320 peak=7421 mean=386 opus_frames=17 opus_bytes=...
```

`samples=0` means the Audio upper-half is not returning capture buffers;
non-zero samples with `peak=0 mean=0` means the I2S input is all zero; an
unchanging full-scale peak points to slot/polarity/data-format trouble.  Speech
should produce a clearly changing peak and mean, while `opus_frames` should be
about 16--17 per second for 60 ms frames.  Disable
`CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS` after microphone bring-up.

## 6. Verify secure WebSocket independently

Before starting the full client, run the small transport-only application:

```text
nsh> wss_test
wss_test: url=wss://api.tenclass.net/xiaozhi/v1/ ...
wss_test: connecting to api.tenclass.net:443/xiaozhi/v1/
wss_test: connected (TLS and WebSocket handshake complete)
wss_test: PASS
```

`PASS` means DNS, TCP, TLS and the HTTP WebSocket Upgrade all completed.  The
test exits immediately after the Upgrade; it does not send XiaoZhi hello or
audio frames.  An alternate URL and optional token can be supplied as:

```text
nsh> wss_test wss://server.example/path token
```

The default timeout is 15 seconds.  During bring-up, certificate verification
is disabled by `CONFIG_CONTEST2026_128_WSS_TEST_TLS_ALLOW_INSECURE`; the warning
is expected.  A connection error printed here is below the XiaoZhi state
machine.  A `PASS` here followed by a failure in `xiaozhi` points instead to
protocol messages, audio upload, or the server session policy.

## 7. Verify GUI, network and XiaoZhi

Confirm networking before starting the application:

```text
nsh> ifconfig
nsh> ping <gateway-or-server>
```

Then launch the configured endpoint:

```text
nsh> xiaozhi
```

Or override it for a development server:

```text
nsh> xiaozhi wss://server.example/xiaozhi/v1/ optional-token
```

If the framebuffer exists but the GUI is blank, inspect MIPI-DSI/panel boot
logs, then verify the framebuffer geometry and LVGL flush callback.  If the
GUI works but voice does not, return to the microphone capture test before
debugging WebSocket framing or the server.

The bring-up defconfig currently enables
`CONFIG_CONTEST2026_128_XIAOZHI_TLS_ALLOW_INSECURE=y`; a warning is printed and
the WSS peer certificate/hostname is not verified.  This is intentional only
for first hardware/network integration.  For a release image, disable it and
set `CONFIG_CONTEST2026_128_XIAOZHI_TLS_CA_PATH` to a PEM CA bundle present on
the target filesystem.

## 8. Source-level debugging

Start OpenOCD with `board/esp32p4-builtin.cfg`, then attach the ESP RISC-V GDB
to the ELF from the image that is actually flashed:

```sh
/home/lxy/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin/riscv32-esp-elf-gdb \
  /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_xiaozhi/nuttx
```

Useful GDB commands and breakpoints:

```gdb
target extended-remote :3333
monitor halt
info threads
thread apply all bt
break board_es8311_initialize
break esp_i2sbus_initialize
break es8311_configure
break xiaozhi_main
continue
```

When the serial console stops, do not assume the CPU is dead.  Halt it and
inspect both cores, the current PC and all task backtraces.  Preserve the first
fault context and use the matching ELF with `addr2line` before rebuilding.

The XiaoZhi main task and its workers have separate stacks.  The defaults are
32 KiB for the main task, 16 KiB for LVGL, 8 KiB for each audio worker, and
24 KiB for the WebSocket/TLS worker.  They are independently configurable by
the `CONTEST2026_128_XIAOZHI_*STACKSIZE` options.  `CONFIG_STACK_COLORATION`
and `CONFIG_STACK_USAGE` are enabled, so run `ps` while the application is
alive and keep a useful margin in the `USED` column; compiler `.su` files only
report individual function frames and cannot prove that a complete LVGL or TLS
call chain fits.

An older image can fault at `fdlist_get_by_index` from the LVGL framebuffer
flush (`ioctl(FBIO_UPDATE)`), with `MTVAL` close to zero.  That image gave all
pthread workers the global 2 KiB default and also contained an ES8311 one-byte
register-read buffer overflow.  The current board port fixes both.  It also
keeps the first 32 descriptors in the preallocated fd table, avoiding a heap
allocated second fd row during GUI/audio/network startup.  If the signature
persists, first verify the flashed `nuttx.bin` hash against the just-built file,
then capture `ps`, the exception EPC/MTVAL, and a backtrace from the matching
ELF.

## 9. Recommended bring-up order

Use this order so each stage has a small failure surface:

1. Boot to NSH and confirm `/dev/fb0` plus both audio nodes.
2. Confirm ES8311 at its I2C address.
3. Run the microphone capture statistic test and fix DIN/clock/analog input.
4. Run `wss_test` and reach `PASS`.
5. Launch `xiaozhi` and verify the LVGL screen.
6. Verify server TTS playback, then microphone upload and server STT.
7. Attach a speaker and verify playback/PA/output routing.
8. Install a CA bundle and disable insecure TLS before release validation.
