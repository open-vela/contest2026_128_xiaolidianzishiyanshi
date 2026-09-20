---
name: esp32p4-debug
description: Diagnose ESP32-P4 development-time boot silence, stopped serial output, illegal instructions, watchdog resets, FPU faults, and stalled driver initialization with OpenOCD/GDB and matching OpenVela build artifacts. Do not use as a XiaoZhi runtime or deployed hardware-agent skill.
---

# ESP32-P4 Debug

Use this skill for evidence-driven development debugging of the ESP32-P4 Function EV Board overlay. Preserve the first fault context, prove the diagnosis against the exact ELF and image on the board, and turn confirmed findings into an Overlay source fix followed by a clean hardware retest.

## Discover the project context first

Never assume a username, home directory, or absolute checkout location. At the start of every debugging session, locate and confirm the OpenVela workspace from the current working directory, the Skill location, or a path supplied by the user. A valid workspace normally contains `nuttx/`, `build.sh`, `vendor/`, and `cmake_out/`; do not select a directory from its name alone. If more than one candidate exists, ask the user which checkout and build tree are connected to the board.

After confirmation, use these session-local names consistently:

- `OPENVELA_ROOT`: confirmed OpenVela workspace root
- `CONTEST_ROOT`: Vendor Overlay repository containing this Skill
- `NUTTX_ROOT`: `$OPENVELA_ROOT/nuttx`
- `OVERLAY_LINK`: `$OPENVELA_ROOT/vendor/espressif`; confirm where the symlink resolves
- `ESP_IDF_ROOT`: current ESP-IDF reference tree; discover it from the active environment or ask the user, then confirm the expected version before comparing code
- `BUILD_DIR`: selected build directory, normally `$OPENVELA_ROOT/cmake_out/esp32p4-function-ev-board_nsh`
- `DEFCONFIG`: `$CONTEST_ROOT/board/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig`
- `BUILD_CONFIG`: `$BUILD_DIR/.config`
- `ELF`: `$BUILD_DIR/nuttx`
- `IMAGE`: `$BUILD_DIR/nuttx.bin`
- OpenOCD board file: `board/esp32p4-builtin.cfg`
- GDB target: `extended-remote :3333`
- Current SIMPLE_BOOT application offset: `0x2000`

These are defaults, not substitutes for inspection. A XiaoZhi build may instead use `cmake_out/esp32p4-function-ev-board_xiaozhi`; derive every artifact path from the build being debugged and never mix its ELF with another image.

This is a development-time coding/debugging skill. It is not part of the XiaoZhi application, must not add a runtime feature, and must not be deployed under `/data/agent/skills/`. This project currently has no AI hardware runtime Skill there.

## Safety and repository boundaries

- Modify the Vendor Overlay by default. Treat NuttX, ESP-IDF, downloaded HAL repositories, and generated build-tree repositories as read-only reference material unless the user explicitly authorizes changes to a named external repository.
- The Overlay and NuttX are separate Git repositories. Inspect their status independently and preserve all existing dirty or untracked files.
- Do not edit repo manifests or `Make.defs` unless the requested fix actually requires them.
- Do not change the defconfig unless explicitly requested. Never regenerate `.config` with `configure.sh`, menuconfig, olddefconfig, refresh, reconfigure, or similar targets. If an experiment requires a configuration change, edit the existing `.config` directly and record the reversible delta.
- If the user owns the serial session, OpenOCD process, or board power, do not restart, reset, flash, or power-cycle it. Ask the user to perform the needed operation.
- Start with read-only target inspection. Use hardware breakpoints and CSR/frame writes only after the read-only evidence supports a specific hypothesis.

## USB and serial permissions

When OpenOCD, GDB, a serial tool, or a flasher reports `permission denied`, `LIBUSB_ERROR_ACCESS`, or another access failure for a JTAG/USB device or `/dev/ttyACM*`/`/dev/ttyUSB*`, do not work around it with repeated `sudo`, world-writable device modes, or running the whole development session as root. First collect the actual device node, ownership, `lsusb` VID:PID, and `udevadm info` properties. Then require the user to add a persistent udev rule appropriate to that device and distribution.

Provide a least-privilege rule using the observed identifiers and the distribution's serial/device-access group, for example:

```udev
SUBSYSTEM=="tty", ATTRS{idVendor}=="<vid>", ATTRS{idProduct}=="<pid>", MODE="0660", GROUP="<serial-group>", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="<vid>", ATTR{idProduct}=="<pid>", MODE="0660", GROUP="<device-group>", TAG+="uaccess"
```

Ask the user to place the rule under `/etc/udev/rules.d/`, run `sudo udevadm control --reload-rules` and `sudo udevadm trigger`, reconnect the board, and re-login if group membership changed. These operations require administrator authority and must be performed or explicitly authorized by the user. Verify unprivileged device access before resuming OpenOCD or serial debugging.

## Required workflow

### 1. Identify the exact build and silicon

Before interpreting an address or touching the target:

1. Read the current defconfig and build `.config`, including board, boot mode, chip revision selection, debug symbols, frame pointers, and optimization settings.
2. Confirm the physical chip revision and ROM revision from the boot log or target registers. Never apply the ESP32-P4 v1.x FPU/CLIC workaround to a v3.x image by assumption; a revision mismatch can fail at `__start` or cause watchdog loops.
3. Record the ELF and image path, size, modification time, and hash. Confirm they came from the same build. If the board image is uncertain, stop source-level conclusions until it is matched or reflashed.
4. Check the ELF sections, symbols, and build metadata. Locate the map file when instruction placement or IRAM/IROM mapping matters.
5. Check Overlay and NuttX Git state separately before editing.

For the present NSH SIMPLE_BOOT configuration, the application image offset is `0x2000`. Still verify `CONFIG_ESPRESSIF_SIMPLE_BOOT` and MCUboot settings in the current `.config`; `0x20000` is an MCUboot slot offset and a successful verify there does not prove the SoC boots that image.

### 2. Capture a read-only OpenOCD/GDB snapshot

Start OpenOCD only when the user is not already managing it. Do not reset the target during the first capture:

```sh
openocd -f board/esp32p4-builtin.cfg \
  -c init -c "esp appimage_offset 0x2000"
```

Resolve `GDB_BIN` with `command -v`, preferring a compatible ESP RISC-V GDB and falling back to `gdb-multiarch`. Do not assume a historical tool version or installation directory still exists:

```sh
GDB_BIN="$(command -v riscv32-esp-elf-gdb || command -v gdb-multiarch)"
test -n "$GDB_BIN"
```

Attach the matching ELF and inspect both cores without altering execution state beyond the halt needed for inspection:

```sh
"$GDB_BIN" -q -batch "$ELF" \
  -ex "set pagination off" \
  -ex "set remotetimeout 10" \
  -ex "target extended-remote :3333" \
  -ex "maintenance flush register-cache" \
  -ex "info threads" \
  -ex "thread apply all info registers pc ra sp mhartid mcause mepc mtval mstatus" \
  -ex "thread apply all bt 16" \
  -ex "detach"
```

Record, at minimum, each hart's PC, SP, RA, `mhartid`, `mcause`, `mepc`, `mtval`, `mstatus`, task/thread identity, and backtrace. For FPU faults also capture `fcsr`, `frm`, and `fflags`. A PC in ROM such as `0x4fc00b10` immediately after `reset halt` is not by itself the fault site.

Live CSRs may describe a secondary exception or may already have been changed by the trap path. Locate and decode the saved first-exception frame on the task or interrupt stack before assigning root cause. Treat live state and saved frame state as separate evidence.

### 3. Resolve the evidence statically

Use the toolchain shipped with the workspace unless the ELF's producer requires another verified toolchain:

```sh
RISCV_TOOL_BIN="$OPENVELA_ROOT/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"
export PATH="$RISCV_TOOL_BIN:$PATH"
riscv-none-elf-addr2line -aife "$ELF" <pc> <ra> <saved-epc>
riscv-none-elf-objdump -dSl "$ELF"
riscv-none-elf-nm -nC "$ELF"
riscv-none-elf-readelf -hSWs "$ELF"
```

Use focused address ranges and symbol searches rather than dumping an entire ELF when possible. Correlate:

- PC/EPC and RA with source lines and surrounding instructions;
- `mtval` with the faulting instruction or bad address;
- symbols and sections with IRAM, IROM, DRAM, and flash mapping state;
- the map/archive member name with linker-script patterns;
- stack base/top/SP and saved frames before claiming stack overflow;
- on-board machine code with the ELF when stale-image loading is plausible.

### 4. Follow the symptom-specific branch

- **No application log or startup reset:** verify silicon revision selection, image offset, entry point, early IRAM-to-IROM calls, and whether the board is repeatedly passing through ROM. O0-only faults require comparing actual sections and disassembly because formerly inlined helpers may become out-of-line IROM calls.
- **Serial output stops:** do not equate UART silence with a dead CPU. Inspect both cores, tasks, interrupt state, and CLIC priority state. On ESP32-P4 v1.x, a stale `mintstatus.MIL` after `mret` can mask UART/timer interrupts while execution continues.
- **Illegal instruction:** resolve EPC and instruction bytes first. Distinguish a true unsupported opcode, chip-revision mismatch, unmapped IROM fetch, and the v1.x EXT_ILL/FPU erratum. Do not skip the instruction as a fix.
- **Watchdog reset:** capture a fault before the next reset using exception/assert/abort hardware breakpoints or saved frames. Look for early boot execution from unmapped application IROM and for repeated execution at the same PC.
- **FPU exception:** capture EXT_ILL reason before it is cleared, plus FS state and FCSR. On v1.x, a legal FPU instruction may trap even when FS is Dirty. Dynamic-rounding instructions can repeatedly trap when `FCSR.frm` is reserved (5-7); FLW/FSW have special detection limitations. Preserve task/core ownership and context-switch semantics.
- **Driver initialization stalls:** bracket the suspected initialization entry and the next success milestone with hardware breakpoints. Check locks, interrupt enablement, clocks/resets, DMA ownership, stack usage, and required device nodes in subsystem order. For XiaoZhi, isolate framebuffer, codec/I2C, microphone/I2S, network/TLS, then application startup rather than debugging all layers at once.

Consult the repository notes when the symptom matches:

- [General debugging](../docs/dev/How-to-debug.md)
- [ESP32-P4 v1.x FPU/CLIC and startup faults](../docs/dev/ESP32P4-v1-FPU-CLIC-debug.md)
- [O0 early-boot IRAM/watchdog fault](../docs/dev/ESP32P4-O0-early-boot-IRAM-debug.md)
- [Chip revision behavior](../docs/dev/ESP32P4-version.md)
- [XiaoZhi bring-up and driver order](../docs/dev/ESP32P4-XiaoZhi-bringup.md)
- [MIPI-DSI driver bring-up](../docs/dev/ESP32P4-MIPI-DSI.md)

### 5. Validate one hypothesis at a time

When static and read-only evidence identify a testable hypothesis, use the smallest reversible experiment:

- place hardware breakpoints at the fault entry and the expected success point;
- record `mhartid` and state on every hit;
- temporarily change a live CSR or the saved exception frame only when their different lifetimes are understood;
- remember that exception return restores saved CSR state, so a live-only edit may disappear;
- never write flash/IROM as ordinary RAM;
- do not treat skipping a result-producing instruction as a functional fix.

If a test requires a reset, OpenOCD restart, serial interaction, or power cycle controlled by the user, pause and request that exact operation. A temporary CSR or memory patch proves or rejects a hypothesis only; it must not be the delivered solution.

### 6. Implement the confirmed fix in the Overlay

Apply the smallest source-level fix in the Vendor Overlay. Prefer board/chip hooks, weak hooks, or Overlay linker rules over changes to NuttX common code. When using ESP-IDF as a reference, port the hardware mechanism into NuttX startup, exception, SMP, and scheduling semantics; do not copy a FreeRTOS lifecycle blindly.

For early boot, prove every called function is available in the currently mapped region using the final map, sections, and disassembly. An IRAM attribute alone does not prove the archive member matched the linker rule.

### 7. Rebuild, flash, and cold-start retest

Use `build.sh` when appropriate. Before direct CMake builds, prepare the required environment:

```sh
mkdir -p /tmp/openvela-ccache /tmp/openvela-ccache-tmp
export CCACHE_DIR=/tmp/openvela-ccache
export CCACHE_TEMPDIR=/tmp/openvela-ccache-tmp
RISCV_TOOL_BIN="$OPENVELA_ROOT/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"
BUILD_TOOLS_BIN="$OPENVELA_ROOT/prebuilts/build-tools/linux-x86_64/bin"
export PATH="$RISCV_TOOL_BIN:$BUILD_TOOLS_BIN:$PATH"
command -v esptool >/dev/null || command -v esptool.py >/dev/null
cmake --build "$BUILD_DIR" -j8
```

If neither esptool command is available, ask the user for the active ESP development environment or tool path instead of guessing a user-specific installation directory.

Do not reconfigure or regenerate `.config`. If linker-script aggregation is stale and a CMake configure pass is genuinely required, explain why and ensure it does not invoke a configuration regeneration path.

Confirm the rebuilt ELF and `nuttx.bin` are a new matching pair, then flash the current image to the offset derived from `.config`:

```sh
openocd -f board/esp32p4-builtin.cfg \
  -c "init; reset halt; program_esp $IMAGE 0x2000 verify; reset run; shutdown"
```

Require `Verify OK`, then perform a full SoC reset or user-assisted cold power cycle. A JTAG-core-only reset can retain stale SRAM state. Confirm the board is running the new machine code/image rather than relying only on flash verification.

The retest must pass the original fault point and reach an explicit milestone: scheduler/idle, NSH, required device-node registration, driver success marker, or XiaoZhi business entry as appropriate. Recheck both the triggering case and a normal boot.

### 8. Report the result

Return a compact evidence trail:

- physical chip/ROM revision and active build configuration;
- matching ELF/image identity and flash offset;
- first saved fault context and static resolution;
- hypothesis test and observed result;
- Overlay files changed, while explicitly noting any authorized external-repository change;
- rebuild, verify, full reset/cold-start result, and the milestone reached;
- remaining uncertainty or user-owned hardware action, if any.

Do not call the issue fixed based only on a clean build, a successful flash verify, a temporary GDB patch, or disappearance of serial output.
