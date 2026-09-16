# ESP32-P4 开发会话说明

本文件只记录工程位置与长期开发规范。具体功能需求以用户在当前会话中的说明为准，不在此文件中固化。详细调试过程记录在 `docs/dev/`。

## 工程与构建位置

- Vendor Overlay 工程根目录：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi`
- OpenVela 工作区根目录：`/home/lxy/openvela`
- NuttX 源码：`/home/lxy/openvela/nuttx`
- Overlay 内容会软链接到：`/home/lxy/openvela/vendor/espressif`
- ESP-IDF v6.0.2：`/home/lxy/.espressif/v6.0.2/esp-idf`
- 当前 CMake 构建目录：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh`
- 当前 defconfig：`/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig`
- 当前构建配置：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/.config`
- 调试 ELF：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx`
- 烧录镜像：`/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`

## OpenOCD 与调试位置

- VS Code 调试配置：`/home/lxy/openvela/.vscode/launch.json`
- RISC-V GDB：`/home/lxy/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin/riscv32-esp-elf-gdb`
- OpenOCD GDB target：`extended-remote :3333`
- 应用镜像 flash offset：`0x2000`
- OpenOCD 启动命令：`openocd -f board/esp32p4-builtin.cfg`
- OpenOCD 烧录命令：`openocd -f board/esp32p4-builtin.cfg -c "init; reset halt; program_esp <nuttx.bin> 0x2000 verify; reset run; shutdown"`
- 静态定位优先使用：`riscv32-esp-elf-addr2line`、`objdump`、`nm`、`readelf`
- ESP32-P4 v1.x FPU/CLIC 调试记录：`docs/dev/ESP32P4-v1-FPU-CLIC-debug.md`

## 长期开发规范

- 默认只修改 Vendor Overlay；只有用户明确授权时才修改 NuttX，且优先使用 weak hook、板级 hook 或独立补丁点。
- Overlay 与 NuttX 是独立仓库并通过软链接协作；检查状态和差异时分别在对应仓库执行。
- 工作区已有改动、日志和未跟踪文件默认属于用户，不覆盖、不清理、不回退。
- 默认使用 CMake 构建；除非构建链明确依赖，否则不要修改 `Make.defs`。
- 每次调试前重新读取当前 defconfig、`.config`、ELF 和链接结果，不依赖旧会话结论。
- 未经明确要求不要修改 defconfig；临时配置实验应说明并保留可恢复路径。
- 增量构建：`cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh -j8`
- 修改 defconfig 后重新配置：`cmake --build /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh --target reconfigure`
- 可使用串口工具烧录，也可在用户已启动 OpenOCD 时通过 GDB/OpenOCD 下载；烧录和调试必须使用同一次构建生成的 ELF/镜像。
- 用户负责串口、OpenOCD 或板卡电源时，不擅自重启服务或执行物理复位；需要时停下来请用户操作。
- 串口停止不等于 CPU 卡死；先通过 GDB 检查双核 PC、寄存器、任务状态和异常现场。
- GDB 内存/CSR 热补丁只用于快速验证假设；结论确认后必须落实到源码、重新构建、烧录并冷启动验证。
- 异常处理中的实时 CSR 可能已被 trap 入口改写；分析原始现场时优先读取保存的异常帧。
- 启动早期 hook 不得调用尚未映射的 IROM 代码；必须结合 map、ELF section 和反汇编确认函数实际落在 IRAM。
- 移植 ESP-IDF 修复时提取芯片机制和寄存器操作，按 NuttX 的启动、异常与调度模型重新落位，不能照搬 FreeRTOS 生命周期。
- 修复验证必须越过原始故障点并到达预期里程碑；启动问题至少验证到 NSH 或目标业务入口。
- 提交前执行格式检查、构建验证和 diff 检查；引用外部实现时优先使用芯片厂商、ESP-IDF 和 NuttX 官方来源。
