# ESP32-P4 v1.x FPU、CLIC 与启动卡死调试记录

## 1. 当前状态

本文记录 2026-09-15 在 ESP32-P4 v1.3 实板上，将 ESP-IDF v6.0.2 的旧版本
silicon FPU workaround 移植到 openvela/NuttX，并通过 OpenOCD/GDB 逐步解决启动
卡死的过程。

截至本文完成时：

- ESP32-P4 v1.3 可以完成 ROM、PSRAM、SPI flash 和 NuttX 初始化；
- draft CLIC 中断返回后，UART 和普通优先级中断可以继续工作；
- MIPI 初始化中的 `FLW`、`fcvt.s.wu`、`fdiv.s` 等 FPU 指令可以正常执行；
- MIPI D-PHY PLL 配置完成；
- `/dev/fb0` 注册成功；
- 系统可以进入 `nsh_main`；
- 最终镜像已经通过 OpenOCD 烧写和 verify。

本次任务由用户明确允许修改 NuttX 公共源码，但要求尽量通过 hook 完成。因此最终
在 NuttX 通用 FPU 执行路径中只加入一个默认空操作的 weak hook；CLIC 只增加扩展
寄存器的配置默认值，其余芯片行为均放在 ESP32-P4 overlay 中。

## 2. 工程和调试环境

| 项目 | 路径或配置 |
| --- | --- |
| OpenVela 根目录 | `/home/lxy/openvela` |
| NuttX 仓库 | `/home/lxy/openvela/nuttx` |
| ESP32-P4 overlay | `/home/lxy/openvela/contest2026_128_xiaolidianzishiyanshi` |
| ESP-IDF 参考源码 | `/home/lxy/.espressif/v6.0.2/esp-idf` |
| 构建目录 | `/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh` |
| 调试 ELF | `cmake_out/esp32p4-function-ev-board_nsh/nuttx` |
| 烧写镜像 | `cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin` |
| GDB | `/usr/bin/gdb-multiarch` |
| OpenOCD GDB 端口 | `3333` |
| App image offset | `0x2000` |
| 实测芯片 | ESP32-P4 revision v1.3 |
| ROM | `esp32p4-eco2-20240710` |

VS Code 调试配置位于：

```text
/home/lxy/openvela/.vscode/launch.json
```

主要参数为：

```json
{
    "target": "extended-remote :3333",
    "executable": "${workspaceFolder}/cmake_out/esp32p4-function-ev-board_nsh/nuttx",
    "gdbpath": "/usr/bin/gdb-multiarch"
}
```

## 3. 最初故障现象

### 3.1 ROM 后反复 WDT reset

早期版本在输出以下日志后复位：

```text
*** Booting NuttX ***
I (...) boot: chip revision: v1.3
W (...) boot.esp32p4: CPU has been reset by WDT.
```

Saved PC 位于应用 IROM 区域。原因是在 application IROM 尚未建立映射时，过早
调用了位于该区域的 `riscv_fpuconfig()`。

### 3.2 PSRAM 初始化后卡住

修正早期调用后，启动可以到达：

```text
I (...) esp_psram: Adding pool of 32768K of PSRAM memory to heap allocator
```

随后没有继续输出。最终确认 cache suspend/resume 路径中的
`esp_cache_utils.c.o` 没有被链接到 IRAM，关闭 cache 后仍从 flash 执行代码而
卡死。

### 3.3 MIPI 初始化中 UART 停止

继续修正后，串口可以到达 MIPI 初始化，但在普通中断发生后停止。JTAG 发现
`mintstatus.MIL` 保持为 `0x3f`，使优先级 1 的 UART、timer 等中断全部被屏蔽。

### 3.4 合法 FPU 指令触发 Illegal Instruction

中断恢复后，最初捕获到：

```text
riscv_exception: EXCEPTION: Illegal instruction
MCAUSE: 00000002
EPC:    4000cd5c
MTVAL:  3007b7f3
```

继续完善 FPU 初始化后，MIPI PLL 计算稳定复现为：

```text
MCAUSE = 0x00000002
EPC    = 0x40026484
MTVAL  = 0xd015f453
```

反汇编结果：

```asm
fcvt.s.wu fs0,a1
```

这是合法的 RV32F 指令，而不是损坏的代码流。

## 4. ESP-IDF v6.0.2 参考实现

分析的主要文件：

```text
components/riscv/vectors.S
components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S
components/freertos/FreeRTOS-Kernel/portable/riscv/port.c
components/riscv/include/riscv/rv_utils.h
components/soc/esp32p4/include/soc/soc_caps.h
```

ESP-IDF 对 ESP32-P4 声明：

```c
#define SOC_CPU_HAS_FPU_EXT_ILL_BUG 1
```

异常入口读取并清除扩展 CSR `0x7f0`：

```asm
csrrw a0, 0x7f0, zero
```

reason bit 定义为：

```text
bit 0: FPU
bit 1: HWLP
bit 2: PIE
bit 3: DSP
```

若 bit 0 表示 FPU，IDF 跳转到 `rtos_save_fpu_coproc`。ESP32-P4 v1.x 还存在
FLW/FSW 不会可靠设置 FPU reason 的缺陷，因此 IDF 额外检查：

- opcode `0x07`：FLW；
- opcode `0x27`：FSW；
- `C.FLW/C.FSW/C.FLWSP/C.FSWSP` 的压缩编码；
- FFLAGS、FRM 和 FCSR，即 CSR 1、2、3。

FreeRTOS 侧不是简单打开全局 FPU，而是使用 lazy coprocessor ownership：

1. 调度器开始前关闭 FPU；
2. 任务第一次使用 FPU 时触发 Illegal Instruction；
3. 异常中设置 `mstatus.FS`，登记当前 owner；
4. 必要时保存旧 owner、恢复新 owner 的寄存器；
5. 使用 FPU 的任务在 SMP 下固定到当前 core。

因此只移植 `vectors.S` 中的异常识别，而不处理任务 FPU 生命周期是不完整的。

## 5. OpenOCD/GDB 排查方法

### 5.1 启动 OpenOCD

用户在本次调试中负责保持 OpenOCD 和串口可用。OpenOCD 启动方式为：

```sh
openocd -f board/esp32p4-builtin.cfg \
  -c init -c reset halt -c "esp appimage_offset 0x2000"
```

需要用户观察新的串口日志、物理复位或重启 OpenOCD 时，应停止实验并明确请求；
OpenOCD 仍能连接时不应擅自重启服务。

### 5.2 先只读检查目标

```sh
/usr/bin/gdb-multiarch -q -batch \
  cmake_out/esp32p4-function-ev-board_nsh/nuttx \
  -ex "set pagination off" \
  -ex "set remotetimeout 10" \
  -ex "target extended-remote :3333" \
  -ex "maintenance flush register-cache" \
  -ex "info threads" \
  -ex "thread apply all info registers pc mcause mtval mstatus sp" \
  -ex "thread apply all bt 12" \
  -ex "detach"
```

目标停在 ROM `0x4fc00b10` 不一定是故障；OpenOCD 使用 `reset halt` 启动时这通常
只是预期的 reset-halt 状态。

### 5.3 用证据断点缩小问题

本次使用的关键方法是同时在异常 hook 和“故障调用之后”设置硬件断点：

```gdb
break esp_fpu_exception
break esp_mipi_dsi.c:683
continue
```

异常入口记录：

```gdb
info registers pc mhartid mcause mtval mstatus fcsr frm fflags
bt
```

必须记录 `mhartid`。不同异常 frame 地址不代表一定发生了任务迁核；本次三次异常
均确认发生在 hart0。

`EXT_ILL CSR 0x7f0` 被 hook 用 `csrrw` 读取后已经清零。因此若在 hook 读取之后
才断住，应检查编译器保存的局部变量 `reason`，不能再直接读取 CSR 并据此判定
reason 原本为零。

### 5.4 热补丁验证原则

GDB 可临时修改 live CSR、PC 或异常 frame，例如：

```gdb
set $mstatus = ...
set $pc = ...
set {unsigned int}ADDRESS = VALUE
```

本次先后验证过：

- FS 已开启时直接返回并重试；
- 在一次异常内执行 `FS Off -> Initial`；
- 在一次异常内执行 `FS Off -> Dirty`；
- 让 FS=Off 跨过一次 `mret`，再在下一次异常中启用。

上述实验都无法单独解决 `fcvt.s.wu` 重复异常，说明根因不只是 FS enable 边沿。

热补丁必须遵守：

- live `mstatus` 与异常 frame 中保存的 `mstatus` 是两份状态；
- NuttX 异常尾部会从 frame 恢复 CSR，只改 live CSR 可能在 `mret` 前被覆盖；
- flash/IROM 不能当作普通 RAM 直接修改；断点通常使用硬件断点资源；
- 跳过产生结果的指令会留下未定义目标寄存器，只能用于控制流验证；
- 实验结果确认后必须回到源码修改、重新构建和烧写。

### 5.5 通过 OpenOCD 烧写

```gdb
monitor reset halt
monitor program_esp /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin 0x2000 verify
monitor reset halt
```

每次烧写后确认：

```text
** Verify OK **
```

调试 ELF 和板上镜像必须来自同一次构建，否则源码行、局部变量和 backtrace 都可能
错误。

## 6. 分阶段排查过程

### 6.1 早期 FPU 初始化引发 WDT

最初将 `riscv_fpuconfig()` 放在 ROM/boot 早期路径。该函数本身位于 application
IROM，而此时应用映射还未就绪，CPU 跳入未映射区域，最终被 WDT 复位。

最终将 FPU 初始化放在 `up_irqinitialize()` 中：应用映射已经完成，同时又早于
NuttX 开始创建和切换普通任务。

### 6.2 cache 工具没有进入 IRAM

链接脚本原模式为：

```ld
*libarch.a:esp_cache.*(...)
```

实际 object 名为 `esp_cache_utils.c.o`，不符合 `esp_cache.` 模式。修改为：

```ld
*libarch.a:esp_cache*.*(...)
```

并同步修改 rev1/rev3 两份 section linker script。修复后 cache suspend/resume
函数位于 IRAM，PSRAM heap 初始化可以继续。

### 6.3 draft CLIC 必须恢复完整 mcause

ESP32-P4 v1.x 的 draft CLIC 在 `mret` 时会从 `mcause.MPIL` 恢复
`mintstatus.MIL`。NuttX 通用异常上下文没有保存完整 `mcause`，导致第一次中断
以后 MIL 保持 `0x3f`，优先级 1 IRQ 全部被阻塞。

最终使用 NuttX 的整数异常上下文扩展 hook：

```text
CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS=y
CONFIG_ARCH_RISCV_INTXCPT_EXTREGS=1
```

`save_extctx` 保存完整 `mcause`，`load_extctx` 在 `mret` 前恢复。

新任务的扩展 `mcause` 不能初始化为零。零值会使第一次 `mret` 进入 U-mode，随后
访问 `mstatus` 等特权 CSR 再次异常。正确初值为：

```text
MPP  = M-mode
MPIL = 0
```

同时修正了 `esp_irq.c` 中 edge IRQ 判断写反的问题：必须与
`INTR_TYPE_EDGE` 比较，而不是 `INTR_TYPE_LEVEL`。

### 6.4 只移植 EXT_ILL 识别仍会 panic

第一版 FPU hook 认为：若 `mstatus.FS` 已经非零，则当前指令一定是真正的非法
指令。这与 IDF 行为不符。

JTAG 在 hook 入口确认：

```text
instruction = 0xd015f453
reason      = 0x1
mstatus     = 0x80007800
FS          = Dirty
```

即硬件明确报告 FPU，指令也确实是合法 `fcvt.s.wu`，但旧判断仍将其转发到
`riscv_exception()`，最终 panic。因此不能用“FS 已开启”排除 P4 v1.x FPU trap。

### 6.5 只切换 FS 会在同一 EPC 无限重入

移除上述判断后，先后尝试 IDF 的 `csrs mstatus, 1 << 13`、显式
Off→Initial、Off→Dirty 和跨 `mret` 的两阶段切换，仍然重复命中：

```text
EPC   = 0x40026484
MTVAL = 0xd015f453
```

这一步证明问题已从“异常被错误转发”变为“动态舍入指令本身仍无法执行”。

### 6.6 最终根因：新任务继承了非法 FCSR.frm

在重复异常入口读取 FCSR：

```text
FCSR   = 0xb4
frm    = 5
fflags = 0x14
```

RISC-V 合法舍入模式为 0 到 4，5 到 7 是保留值。`FLW` 不读取舍入模式，因此能
正常执行；`fcvt.s.wu` 的指令编码为 `rm=111`，即动态读取 FCSR.frm，遇到
`frm=5` 后会触发 Illegal Instruction。

最终在任务第一次由 FS=Off 进入 FPU hook 时执行：

```c
SET_CSR(CSR_STATUS, MSTATUS_FS_INIT);
WRITE_CSR(CSR_FCSR, 0);
```

这会将舍入模式初始化为 RNE，并清除异常标志。已有保存上下文的任务不清零 FCSR，
避免破坏任务自己的舍入模式和异常状态。

## 7. 最终 NuttX 适配方案

### 7.1 新任务 FPU 状态

复用 `riscv_initial_extctx_state()` hook：

- 初始化 draft CLIC 的扩展 `mcause`；
- ESP32-P4 v1.x 新任务的 `mstatus.FS` 清为 Off。

这样任务第一次执行 FPU 指令时会进入 EXT_ILL hook，与 IDF 的 lazy coprocessor
启动方式一致。

### 7.2 Illegal Instruction hook

在 `riscv_exception_attach()` 之后，只覆盖 Illegal Instruction IRQ slot：

```c
irq_attach(RISCV_IRQ_IINSTRUCTION, esp_fpu_exception, NULL);
```

hook 的处理顺序为：

1. 读取 `mtval` 指令；
2. 读取并清除 `EXT_ILL`；
3. 根据 reason 和指令编码识别 FPU；
4. 非 FPU 指令转发给原 NuttX exception handler；
5. 启用 live FS 和保存的返回 FS；
6. 首次使用时初始化 FCSR；
7. 不增加 EPC，返回后重试原指令。

### 7.3 FPU restore weak hook

从 FS=Off 的任务切换到一个具有已保存 FPU 上下文的任务时，NuttX 会在异常路径中
执行 `FLW` 恢复寄存器。此时必须先启用当前 core 的 live FPU。

因此在 NuttX 通用代码中加入：

```c
void riscv_prepare_restorefpu(uintreg_t *regs);
```

通用汇编提供 weak 空实现，所有其它 RISC-V 平台行为不变；ESP32-P4 提供强实现，
只在 incoming context 为 Clean/Dirty 时设置 live FS。异常返回随后会安装 incoming
task 保存的 FS 状态。

## 8. 修改文件

### 8.1 NuttX 仓库

| 文件 | 修改内容 |
| --- | --- |
| `arch/risc-v/Kconfig` | P4 v1.x 默认分配一个扩展整数上下文寄存器 |
| `arch/risc-v/src/common/riscv_fpu.S` | 增加 weak `riscv_prepare_restorefpu` 空 hook |
| `arch/risc-v/src/common/riscv_internal.h` | FPU restore 前调用 prepare hook |

### 8.2 ESP32-P4 overlay

| 文件 | 修改内容 |
| --- | --- |
| `chip/esp32p4/Kconfig` | rev<3 配置选择异常上下文扩展 |
| `chip/esp32p4/esp_fpu.c` | EXT_ILL 分类、首次 FCSR 初始化、restore hook |
| `chip/esp32p4/esp_fpu.h` | FPU 初始化接口 |
| `chip/esp32p4/esp_clic.c` | 新任务 mcause 与 FS 初始状态 |
| `chip/esp32p4/riscv_extctx.S` | 保存和恢复完整 mcause |
| `chip/esp32p4/espressif/esp_irq.c` | 安装 FPU hook，修正 edge IRQ 判断 |
| `chip/esp32p4/espressif/CMakeLists.txt` | CMake 构建加入新源文件 |
| `chip/esp32p4/espressif/Make.defs` | Make 构建加入新源文件 |
| `board/esp32p4/common/scripts/esp32p4_sections.ld` | 修正 cache object IRAM 匹配 |
| `board/esp32p4/common/scripts/esp32p4_sections.rev3.ld` | 同步修正 rev3 链接脚本 |

`chip/esp32p4/esp_mipi_dsi.c` 在任务开始前已有用户自己的未提交修改，本次
FPU/CLIC 修复没有覆盖或还原该文件。

## 9. 构建与烧写

已验证构建命令：

```sh
CCACHE_DIR=/tmp/openvela-ccache \
CCACHE_TEMPDIR=/tmp/openvela-ccache-tmp \
PATH=/home/lxy/openvela/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin:$PATH \
cmake --build \
  /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh -j4
```

生成文件：

```text
/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx
/home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin
```

OpenOCD 烧写：

```gdb
monitor reset halt
monitor program_esp /home/lxy/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin 0x2000 verify
monitor reset halt
```

当前构建仍会输出 ELF RWX LOAD segment 和 esptool unknown section type 警告，
但镜像可以生成并启动。ROM 的 SHA-256 expected 全零提示也不是本次启动卡死根因。

## 10. 实板验证

### 10.1 FPU 指令越过验证

修复前：

```text
Breakpoint: esp_fpu_exception
EPC:        0x40026484
MTVAL:      0xd015f453
```

修复后只在首次 `FLW` 使用时进入 hook：

```text
EPC:     0x400237b2
MTVAL:   0xb4c7a507
mstatus: FS=Off
FCSR:    0xb4
```

hook 初始化 FCSR 后，第二个断点直接命中：

```text
esp_mipi_dsi.c:683
MIPI: DSI PHY PLL configured
```

说明 `fcvt.s.wu`、后续 `fdiv.s` 和浮点比较均已成功执行，不再重复进入同一 EPC。

### 10.2 framebuffer 验证

JTAG 命中：

```text
esp32p4_bringup.c:178
MIPI: /dev/fb0 registered
```

调用栈：

```text
esp_fb_init_thread
nxtask_start
```

### 10.3 NSH 验证

重新 reset 后命中：

```text
nsh_main(argc=1, ...)
PC = 0x40018f3e
hart = 0
```

证明系统已经完成主要启动流程并进入 NSH。

## 11. 可复用经验

1. 串口停止输出不等于 CPU 卡死。先用 OpenOCD 区分 reset-halt、断点 halt、异常
   循环和真正的等待。
2. 调试异常时应同时观察 EPC、`mtval`、`mcause`、`mstatus`、FCSR 和 hart，不能
   只依赖 panic backtrace。
3. 先在故障入口和成功出口设置断点，可直接证明“是否越过原卡点”。
4. IDF 中依赖 FreeRTOS 的 workaround 不能只复制异常汇编，还要理解任务状态、
   owner、SMP 和上下文恢复语义。
5. live CSR 与保存的异常 frame 必须分别修改和验证。
6. 对同一 EPC 的连续异常，应记录每次状态变化。FS 已正确变化但异常不消失时，应
   转向检查 FCSR、权限、指令扩展和任务上下文，而不是继续重复相同的 enable 操作。
7. IRAM attribute 不代表最终一定链接到 IRAM；必须检查 archive member 名称与
   linker pattern 是否匹配。
8. 早期启动代码要先确认其 VMA 所在区域已经映射。
9. GDB 热补丁只能用于验证假设；最终结果必须来自源码补丁、重新构建、烧写和
   reset 后的实板复测。
10. 工作区含多个 Git 仓库和 symlink overlay。修改前后分别检查真实仓库状态，
    并保护用户已有 dirty 文件。
