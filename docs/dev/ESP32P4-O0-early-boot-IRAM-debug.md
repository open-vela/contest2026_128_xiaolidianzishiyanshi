# ESP32-P4 O0 优化启动重启问题排查与修复

## 1. 问题概述

本文记录 2026-09-16 在 ESP32-P4 实板上排查以下问题的过程：

- 选择 `CONFIG_DEBUG_NOOPT=y`，即 O0 优化时，系统启动后反复复位；
- 使用 O1 或 O2 时系统可以正常工作；
- 当前工程还包含 ESP32-P4 v1.x FPU/CLIC workaround。

最终确认问题不是 O0 编译器错误，也不是中断栈溢出，而是 O0 下本应内联的
`up_irq_save()` 和 `up_irq_restore()` 被生成为独立函数，并被链接到了应用
Flash/IROM。启动早期 IROM 映射尚未建立，IRAM 中的 bootloader 路径调用这些
函数时发生取指异常，最终由 watchdog 复位。

修复方式是在 ESP32-P4 overlay 链接脚本中，将 `os.c.o` 产生的
`.text.up_irq_save` 和 `.text.up_irq_restore` 显式放入 IRAM。

## 2. 工程和调试环境

| 项目 | 路径或配置 |
| --- | --- |
| OpenVela 根目录 | `/home/lxy/openvela` |
| Vendor Overlay | `/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi` |
| NuttX | `/home/lxy/openvela/nuttx` |
| 构建目录 | `/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh` |
| 调试 ELF | `cmake_out/esp32p4-function-ev-board_nsh/nuttx` |
| 烧录镜像 | `cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin` |
| OpenOCD GDB target | `extended-remote :3333` |
| 启动模式 | `CONFIG_ESPRESSIF_SIMPLE_BOOT=y` |
| 实际 app offset | `0x2000` |
| 优化配置 | `CONFIG_DEBUG_SYMBOLS=y`、`CONFIG_DEBUG_NOOPT=y` |

当前 O0 编译命令不带 `-O` 参数，并保留 `-fno-omit-frame-pointer` 和 `-g3`。

## 3. 第一轮现场：不要把 ROM PC 当作故障点

OpenOCD 初次 halt 时，CPU0 位于：

```text
PC = 0x4fc00b10
SP = 0
RA = 0
```

该地址属于 Boot ROM reset path，不能据此判断应用的故障函数。反复重启时随机
halt 很容易只抓到 ROM 窗口，因此需要继续运行并在异常、assert、abort 等入口
设置断点，或者在目标再次停止后读取 NuttX 保存的异常 frame。

本次曾尝试在以下函数设置硬件断点：

```gdb
hbreak riscv_exception
hbreak abort
hbreak _assert
continue
```

目标没有进入通用 `riscv_exception()`，但随后暂停时已经能看到异常分发现场。

## 4. 二次现场与原始异常 frame

暂停后表面 PC 为：

```text
PC     = 0x4ff02b38 <up_set_interrupt_context+2>
MCAUSE = 0x30000007
MTVAL  = 0x000000a8
MEPC   = 0x4ff02b38
```

回溯为：

```text
up_set_interrupt_context
riscv_doirq_top
riscv_doirq
riscv_dispatch_irq
exception_common
```

该现场一度看起来像中断栈写入失败，但中断栈范围和 SP 为：

```text
g_intstackalloc = 0x4ff1b5a0
g_intstacktop   = 0x4ff1bda0
SP              = 0x4ff1bce8
```

2 KiB 中断栈当时只使用约 184 字节，因此可以排除中断栈耗尽。

ESP32-P4 draft CLIC workaround 会保存和恢复 `mcause`，而当前 PC 还可能是异常处理
过程中发生的二次故障，所以不能只看 live CSR。必须读取第一次异常保存在任务栈
中的 frame。本次 frame 地址为 `0x4ff1f158`，关键内容为：

```text
saved EPC     = 0x400003c0
saved RA      = 0x4ff10944
saved SP      = 0x4ff1f264
saved MSTATUS = 0x00001800
saved MCAUSE  = 0x30000002
```

`MCAUSE` 低位为 2，即 Illegal Instruction。原始故障点不是
`up_set_interrupt_context()`，而是 `0x400003c0`。

## 5. 静态定位与完整调用链

使用 `addr2line` 定位：

```sh
riscv-none-elf-addr2line -aife nuttx \
  0x400003c0 0x4ff10944
```

结果为：

```text
0x400003c0: up_irq_save, arch/risc-v/include/irq.h
0x4ff10944: nuttx_enter_critical, .../nuttx/src/platform/os.c
```

故障 ELF 中的反汇编为：

```asm
4ff10934 <nuttx_enter_critical>:
  ...
4ff1093c: auipc ra,0xf00f0
4ff10940: jalr  -1404(ra)  # 400003c0 <up_irq_save>
4ff10944: sw    a0,-20(s0)
```

继续检查故障任务栈，得到完整启动调用链：

```text
bootloader_init
  -> bootloader_hardware_init
  -> regi2c_ctrl_write_reg_mask
  -> periph_rcc_acquire_enter
  -> periph_rcc_enter
  -> nuttx_enter_critical       [IRAM]
  -> up_irq_save                [Flash/IROM，故障]
```

此时还观察到：

```text
g_running_tasks[0] = NULL
```

说明异常发生在调度器启动之前，正处于 bootloader hardware init 阶段。该阶段不能
调用尚未建立映射的应用 IROM 代码。

## 6. 为什么 O1/O2 正常而 O0 失败

`up_irq_save()` 在 NuttX 中声明为 `static inline_function`。GCC 配置定义位于
`nuttx/include/nuttx/compiler.h`：

```c
#ifdef CONFIG_DEBUG_NOOPT
#  define always_inline_function inline
#  define inline_function inline
#else
#  define always_inline_function \
     __attribute__((always_inline,no_instrument_function)) inline
#  define inline_function __attribute__((always_inline)) inline
#endif
```

NuttX 在 O0 下有意取消 `always_inline`，避免强制内联导致栈使用继续增大。因此：

- O1/O2：`up_irq_save()` 被内联进 IRAM 中的 `nuttx_enter_critical()`；
- O0：编译器生成独立的 `.text.up_irq_save`，默认落入 Flash/IROM；
- bootloader 启动早期从 IRAM 跳入尚未映射的 IROM，产生 Illegal Instruction。

`up_irq_restore()` 具有相同风险，只是系统先在 enter 路径崩溃，尚未执行到 exit
路径。

## 7. 链接脚本缺口与修复

原链接脚本只把两个外层函数放入 IRAM：

```ld
*libarch.a:os.*(.literal.nuttx_enter_critical .text.nuttx_enter_critical)
*libarch.a:os.*(.literal.nuttx_exit_critical .text.nuttx_exit_critical)
```

O0 生成的静态 helper section 没有被匹配。最终在以下两个 overlay 文件中同步增加：

```text
board/esp32p4/common/scripts/esp32p4_sections.ld
board/esp32p4/common/scripts/esp32p4_sections.rev3.ld
```

新增规则：

```ld
*libarch.a:os.*(.literal.up_irq_save .text.up_irq_save)
*libarch.a:os.*(.literal.up_irq_restore .text.up_irq_restore)
```

这里仅移动 `os.c.o` 中的两个 helper，没有把所有 O0 生成的 `up_irq_save()` 副本
全局搬入 IRAM，也没有修改 NuttX 公共 `compiler.h`。这样既修复启动路径，又避免
无谓增加 IRAM 占用或改变全局 O0 内联策略。

## 8. 构建和静态验证

修改链接脚本后，普通增量构建只重新生成了镜像，没有重新生成合并链接脚本。
需要重新运行 CMake 配置：

```sh
cmake -S /home/lxy/openvela/nuttx \
  -B /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh

cmake --build \
  /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh -j8
```

若重新配置后归档工具使用了非绝对路径，需要保证工程工具链目录在 `PATH` 中：

```text
/home/lxy/openvela/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin
```

构建成功后检查 map：

```text
.text.nuttx_enter_critical  0x4ff10934
.text.nuttx_exit_critical   0x4ff109a4
.text.up_irq_save           0x4ff10a0e
.text.up_irq_restore        0x4ff10a2a
```

修复后反汇编：

```asm
4ff10934 <nuttx_enter_critical>:
  ...
4ff1093c: jal 4ff10a0e <up_irq_save>

4ff109a4 <nuttx_exit_critical>:
  ...
4ff10a02: jal 4ff10a2a <up_irq_restore>
```

调用者和 helper 现在全部位于 `0x4ff...` IRAM 区域。

## 9. 烧录偏移陷阱

本次最初按旧记录将镜像烧到 `0x20000`。OpenOCD 报告 write 和 verify 成功，但
SoC 复位后仍加载旧机器码。读取目标内存发现：

```asm
0x4ff1093c: auipc ...
0x4ff10940: jalr  ... # 0x400003c0
```

这证明“烧录校验成功”不等于“启动了该镜像”。检查当前 `.config` 后确认：

```text
CONFIG_ESPRESSIF_SIMPLE_BOOT=y
# CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT is not set
```

ESP32-P4 的烧录规则为：

- `SIMPLE_BOOT`：app offset 为 `0x2000`；
- MCUboot primary OTA slot：默认 app offset 才是 `0x20000`。

因此本配置正确命令为：

```gdb
monitor program_esp \
  /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin \
  0x2000 verify
```

烧录后还需进行完整 SoC reset。当前 OpenOCD 的普通 `reset run` 只报告
`JTAG CPU reset`，旧 SRAM 异常现场仍然存在。本次临时跳转到 ROM
`software_reset()` 完成软件复位：

```gdb
set $pc = 0x4fc00094
continue
```

实际使用时也可以由用户冷启动板卡，效果更明确。

## 10. 实板最终验证

在 `0x2000` 烧录、verify 并执行完整软件复位后：

- 不再发生 HP WDT 重启循环；
- 板上 `0x4ff1093c` 实际机器码已变为 `jal 0x4ff10a0e`；
- `g_running_tasks[0]` 非空；
- 当前任务名为 `CPU0 IDLE`，PID 为 0；
- CPU 位于正常的 WFI idle 路径；
- 回溯已越过原始故障点并进入 NuttX 调度器：

```text
rv_utils_wait_for_intr
esp_cpu_wait_for_intr
esp_pm_impl_waiti
up_idle
nx_start
__esp_start
__start
```

调试任务没有连接串口终端，因此没有直接采集 NSH 提示符，但调度器、idle task
和板上新机器码均已确认，且复位循环已经消失。

## 11. 可复用排查原则

1. O0 独有故障应优先比较 ELF section 和实际反汇编，不能只从 C 源码推断执行位置。
2. 启动早期的 IRAM 函数必须继续审计其所有非内联调用；O1/O2 内联成功不能证明
   O0 安全。
3. live `mcause/mepc` 可能来自二次异常或被 CLIC 返回路径改写，应优先分析保存的
   第一次异常 frame。
4. 在宣称栈溢出前，应同时核对 stack base、top 和 SP；本次中断栈余量充足。
5. 烧录地址必须以当前 `.config` 的启动模式为准，不应把 MCUboot OTA offset 套用
   到 SIMPLE_BOOT。
6. 烧录后应读取板上关键机器码，确认新镜像确实被加载，不能只依赖 flash verify。
7. 修复必须越过原始故障点；启动类问题至少应验证到调度器、NSH 或目标业务入口。
