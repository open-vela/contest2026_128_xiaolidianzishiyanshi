# ESP32P4 CMake 链接脚本 LD_SCRIPT 列表问题排查与修复

## 故障现象

ESP32P4 (esp32p4-function-ev-board_nsh) 使用 CMake 构建时，报错：

```
CMake Error at CMakeLists.txt:706 (get_filename_component):
  get_filename_component unknown component
  /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/esp_rom/esp32p4/ld/esp32p4.rom.eco5.libc.ld
```

**原因**：`nuttx/CMakeLists.txt:702` 通过 `get_property(ldscript GLOBAL PROPERTY LD_SCRIPT)` 获取的 `ldscript` 是一个分号分隔的 CMake 列表（包含 13 个 `.ld` 文件），而 `nuttx/CMakeLists.txt:706` 的 `get_filename_component(LD_SCRIPT_NAME ${ldscript} NAME)` 把整个列表当作单个路径解析，从第二个元素开始被误认为是 `NAME` 组件的参数，导致报错。

实际上 NuttX 的 CMake 构建系统对 LD_SCRIPT 的多文件场景完全没有处理：

1. `get_filename_component(LD_SCRIPT_NAME ${ldscript} NAME)` — 把列表当单路径解析，报错
2. `nuttx_generate_preprocess_target(SOURCE_FILE ${ldscript} ...)` — 只预处理第一个文件
3. `target_link_options(nuttx PRIVATE -T${ldscript})` — 把整个列表作为单个字符串传给链接器

实际 LD_SCRIPT 内容（13 个文件）：

```
# 来自 chip hal_esp32p4.cmake 的 10 个 ROM 链接脚本：
esp32p4.rom.eco5.ld
esp32p4.rom.eco5.libc.ld
esp32p4.rom.eco5.libgcc.ld
esp32p4.rom.eco5.newlib.ld
esp32p4.rom.api.ld
esp32p4.rom.version.ld
esp32p4.rom.libc-suboptimal_for_misaligned_mem.ld
esp32p4.rom.systimer.ld
esp32p4.peripherals.ld
rom.api.ld

# 来自 board CMakeLists.txt 的 3 个 board 链接脚本：
esp32p4_aliases.ld
esp32p4_flat_memory.ld
esp32p4_sections.rev3.ld
```

## 背景知识

### LD_SCRIPT 全局属性

NuttX CMake 构建系统使用 `LD_SCRIPT` 全局属性来收集所有需要的链接脚本。多个 CMake 文件通过 `set_property(GLOBAL APPEND PROPERTY LD_SCRIPT ...)` 往里追加文件路径。最终在 `nuttx/CMakeLists.txt:702` 获取该属性，预处理后传递给链接器。

### NuttX 上游的修复

NuttX 上游已经修复了这个问题（[apache/nuttx@f5291a8](https://github.com/apache/nuttx/blob/f5291a8df19660fc69992c0763e1a4cc87b7037c/CMakeLists.txt#L655-L674)），用 `foreach` 遍历列表中的每个文件分别预处理和传递给链接器。但 openvela 的 NuttX 尚未合入此修复，且不能直接修改 openvela 的 nuttx 仓库。

### ESP HAL 3rdparty 的引入

ESP32P4 的底层硬件支持来自 Espressif 官方的 `esp-hal-3rdparty` 仓库（类似 ESP-IDF 的精简版）。它通过以下路径引入构建系统：

- **chip 层 CMakeLists.txt**：`vendor/espressif/chips/esp32p4/espressif/CMakeLists.txt`（对应 `nuttx/arch/risc-v/src/common/espressif/Make.defs:212` 的 CMake 版本）
- 该文件第 396 行 `include(${NUTTX_CHIP_ABS_DIR}/hal_${CHIP_SERIES}.cmake)` 引入了 HAL 配置

### hal_esp32p4.cmake 的功能

`chip/esp32p4/hal_esp32p4.cmake`（对应 `nuttx/arch/risc-v/src/common/espressif/Make.defs:212`）负责：

1. **头文件路径**：设置 `esp-hal-3rdparty` 各组件的 include 目录（约 200+ 条路径）
2. **ROM 链接脚本**（第 193-235 行）：根据芯片版本（eco5/eco3 等）和配置选项，构建 ROM ld 文件列表并追加到 `LD_SCRIPT` 全局属性
3. **HAL 源文件**：收集 `esp-hal-3rdparty` 各组件的 `.c` 文件加入编译

ROM 链接脚本的内容是纯文本的符号地址定义，例如：
```ld
rtc_get_reset_reason = 0x4fc00018;
ets_printf = 0x4fc00024;
```

### Board CMakeLists.txt 的功能

`board/esp32p4/esp32p4-function-ev-board/src/CMakeLists.txt` 负责：

1. **板级源文件**：boot、bringup、GPIO 等板级驱动
2. **Board 链接脚本**（第 53-66 行）：根据版本检查选择 sections.ld 或 sections.rev3.ld，追加到 `LD_SCRIPT`

Board 链接脚本使用 C 预处理器指令（`#include`、`#if` 、注释等），需要预处理后才能传给链接器。

### 链接脚本的两种类型

1. **ROM 链接脚本**（来自 chip 层 `hal_esp32p4.cmake`）：纯文本，定义 ROM 函数符号地址，不需要 C 预处理。
2. **Board 链接脚本**（来自 board 层 `CMakeLists.txt`）：使用 C 预处理器指令（`#include <nuttx/config.h>`、`#if CONFIG_xxx` 等），需要预处理后才能传给链接器。

### 编译顺序

CMake 的 `add_subdirectory` 决定了处理顺序：**chip 先于 board**。因此 `LD_SCRIPT` 先被 chip 追加 10 个 ROM 文件，再被 board 追加 3 个 board 文件。

## 排查过程

### 第一次尝试：只改 board 的链接脚本

最初认为问题只在于 board 的 3 个链接脚本，尝试创建 combined ld 文件用 `#include` 把它们合并：

```
board/esp32p4/common/scripts/
├── esp32p4_combined_nosections.ld      # aliases + flat_memory
├── esp32p4_combined_sections.ld        # aliases + flat_memory + sections.ld
└── esp32p4_combined_sections.rev3.ld   # aliases + flat_memory + sections.rev3.ld
```

然后修改 board CMakeLists.txt 根据版本检查选择对应的 combined 文件。

**结果**：失败。编译输出显示 LD_SCRIPT 包含 13 个文件，board 的 3 个只是其中一部分，还有 10 个 ROM 链接脚本来自 chip 层。

### 第二次尝试：定位 ROM 链接脚本来源

通过全文搜索 `target_linker_script` 和 `LD_SCRIPT`，追踪到 ROM 链接脚本的实际设置位置：

- `chip/esp32p4/hal_esp32p4.cmake:193-235` — 构建 ROM ld 文件列表并追加到 LD_SCRIPT
- `board/.../src/CMakeLists.txt:66` — 追加 board ld 文件到 LD_SCRIPT

同时查到上游 NuttX 的修复方案：[apache/nuttx@f5291a8](https://github.com/apache/nuttx/blob/f5291a8df19660fc69992c0763e1a4cc87b7037c/CMakeLists.txt#L655-L674)，用 `foreach` 遍历列表分别处理。但 openvela 未合入此修复，且不能直接修改 nuttx 仓库。

### 第三次尝试：确认编译顺序

在两处 `set_property` 前后添加 `message(STATUS ...)` 调试信息，编译后确认：

**CHIP 先执行，BOARD 后执行**

```
[CHIP-DEBUG]  LD_SCRIPT BEFORE = (空)
[CHIP-DEBUG]  LD_SCRIPT AFTER  = 10个ROM文件
[BOARD-DEBUG] LD_SCRIPT BEFORE = 10个ROM文件（chip已设置）
[BOARD-DEBUG] LD_SCRIPT AFTER  = 13个文件（ROM + board）
```

## 最终解决方案

既然 board 在 chip 之后执行，且 board 是最后一处追加 LD_SCRIPT 的地方，只需修改 board 的 CMakeLists.txt，在追加完成后把所有文件合并成一个：

```cmake
# 收集所有脚本：chip 的 ROM 脚本 + board 脚本
get_property(_all_ld_scripts GLOBAL PROPERTY LD_SCRIPT)
list(APPEND _all_ld_scripts ${LDSCRIPTS})

# 拼接为单个文件
set(_combined_ld "${CMAKE_BINARY_DIR}/esp32p4_all.ld")
file(WRITE ${_combined_ld} "")
foreach(_ld ${_all_ld_scripts})
  file(READ ${_ld} _ld_content)
  file(APPEND ${_combined_ld} "${_ld_content}\n")
endforeach()

# 替换 LD_SCRIPT 为单个合并文件
set_property(GLOBAL PROPERTY LD_SCRIPT ${_combined_ld})
```

合并后的文件在编译后位于`cmake_out/BOARD_NAME_CFG_NAME/esp32p4_all.ld`

### 修改的文件

| 文件 | 修改内容 |
|------|----------|
| `board/esp32p4/esp32p4-function-ev-board/src/CMakeLists.txt` | 在结尾处，合并所有LD_SCRIPT文件为一个 |
| `chip/esp32p4/hal_esp32p4.cmake` | 无修改 |
| `nuttx/CMakeLists.txt` | 无修改 |

### 关键点

1. **只改了 board CMakeLists.txt**，不涉及 nuttx 和 chip 的修改
2. **保留了版本检查逻辑**，根据 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` 选择 sections.ld 或 sections.rev3.ld
3. **合并顺序**：ROM 脚本在前（纯文本），board 脚本在后（带预处理指令），NuttX 预处理时会正确处理 `#include` 和 `#if`
4. **最终 LD_SCRIPT 只有一个文件**：`${CMAKE_BINARY_DIR}/esp32p4_all.ld`

## 相关代码位置

| 文件 | 行号 | 说明 |
|------|------|------|
| `nuttx/CMakeLists.txt` | 702 | `get_property(ldscript GLOBAL PROPERTY LD_SCRIPT)` |
| `nuttx/CMakeLists.txt` | 706 | `get_filename_component` 只取第一个文件名（报错点） |
| `nuttx/CMakeLists.txt` | 709 | `nuttx_generate_preprocess_target` 只预处理一个文件 |
| `nuttx/CMakeLists.txt` | 760 | `target_link_options` 传递 `-T${ldscript}` |
| [上游修复](https://github.com/apache/nuttx/blob/f5291a8df19660fc69992c0763e1a4cc87b7037c/CMakeLists.txt#L655-L674) | 655-674 | 上游用 `foreach` 遍历列表的修复 |
| `vendor/espressif/chips/esp32p4/espressif/CMakeLists.txt` | 396 | `include(hal_esp32p4.cmake)` 引入 HAL |
| `chip/esp32p4/hal_esp32p4.cmake` | 193-235 | ROM ld 文件列表构建与追加 |
| `board/.../src/CMakeLists.txt` | 53-83 | board ld 文件 + 合并逻辑 |
