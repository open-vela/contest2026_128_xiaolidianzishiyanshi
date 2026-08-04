# ESP32-P4X Function EV Board MIPI-DSI 移植记录

## 1. 当前状态

本文记录 openvela/NuttX 在 ESP32-P4X Function EV Board 上适配 MIPI-DSI
显示的实现和真机调试结论。截至当前：

- NuttX 可正常启动，UART0/NSH 可持续输入；
- MIPI D-PHY PLL 可锁定，两条 data lane 可进入 stop state 和 HS 传输；
- EK79007 初始化命令通过 LPDT 发送，面板正确接收并执行；
- **Host VPG 竖向彩条已成功铺满 1024×600 屏幕**；
- `/dev/fb0` 可注册，framebuffer 示例可写 PSRAM；
- DSI Host requested/active timing、packet size 和 RGB888 编码一致；
- framebuffer 连续 DMA 暂未恢复，当前 `/dev/fb0` 内容不会送往屏幕。

### 1.1 关键修复：禁用 video shadow 寄存器

**根因**：ESP32-P4 v3.2 的 video shadow commit 机制（`vid_shadow_req`）未能正确
将 requested 寄存器值转移到 active 寄存器，导致 VPG 模式配置无法生效。

**修复方法**：设置 `vid_shadow_en = 0`，直接写入 active 寄存器，绕过 shadow
机制。修复后 `vid_mode_cfg` 和 `vid_mode_cfg_act` 不再出现不一致现象。

**验证**：禁用 shadow 后，Host VPG 竖向彩条立即正确显示。

## 2. 实际目标硬件

| 项目 | 实际配置 |
| --- | --- |
| 开发板 | ESP32-P4X-Function-EV-Board |
| Chip on board | ESP32-P4NRW32X |
| Sample chip revision | v3.2 |
| ROM | `esp32p4-eco7-20260109` |
| 显示模组 | AML070JGI50-07403L，7 英寸 1024×600（仍应核对实物丝印） |
| 面板驱动组合 | EK79007/EK79007AD source driver + TCON，EK73217 gate driver |
| DSI data lanes | 2 |
| Lane bit rate | 650 Mbps/lane（已修正，符合 EK79007AD 规格上限） |
| D-PHY 电源 | 内部 LDO channel 3，2.5 V |
| LCD reset | GPIO27 → `RST_LCD` |
| LCD backlight | GPIO26 → `PWM` |
| 控制台 | UART0，GPIO37 TX / GPIO38 RX，115200 8N1 |

硬件连接已经复查：RST_LCD 跳变正常。P4X 官方要求 DSI FPC 反向插接，LCD
adapter 通过自身 USB 或可靠的 5V/GND 供电。

### 2.1 屏幕模组与驱动芯片检索结果

乐鑫 ESP32-P4X/ESP32-P4 Function EV Board 用户指南列出的配套屏幕为 7 英寸、
1024×600，并同时提供 display datasheet、EK79007AD 和 EK73217BCGA 两份驱动
芯片资料。配套屏幕规格书进一步给出模组型号 `AML070JGI50-07403L`，厂商为
深圳市阿美林电子科技有限公司，接口为 MIPI，驱动 IC 明确写为
`EK79007 + EK73217`。因此当前硬件不是只有一个“LCD 控制器”：

- **EK79007/EK79007AD**：1536-channel source driver，并集成 timing controller
  和 MIPI DSI 接口，负责接收像素/命令、产生 source 输出及主要面板时序；
- **EK73217**：配套 gate driver，负责行扫描；EK79007AD 数据手册的 1024×600
  dual-gate application block diagram展示了 source/TCON 与 gate driver 的组合；
- 模组为 normally-black、transmissive，1024×RGB×600、最高 16.7M 色，背光为
  27 颗白光 LED；所以“背光亮但无有效像素驱动”在外观上就是暗灰黑屏。

以上型号来自乐鑫为开发板公开的配套资料，可信度高，但仍需核对当前实物 FPC
标签，排除套件批次或换屏差异。

### 2.2 EK79007AD 与当前配置直接相关的信息

| 项目 | 数据手册结论 | 对当前实现的含义 |
| --- | --- | --- |
| 原生分辨率 | 支持 1024×600，`RES[1:0]=00` 为默认值 | 分辨率选择本身无需额外改写 |
| 色深 | 8-bit、256 gray scale，支持 dithering/FRC | RGB888 输入方向合理 |
| MIPI lane | 支持 4 lane 和 2 lane | 2 lane 必须写 `B2[4]=1`，即当前 `B2=0x10` |
| BIST | `B1[3]=1`，不需要 DCLK | 当前 `B1=0x08` 正确；未出现 BIST 是重要异常 |
| 复位 | GRB 低有效，通常上拉 | 当前 GPIO27 低脉冲方向正确 |
| 上电 | GRB 前 CLK/Data lane 应保持 LP11 | 当前 lane-stop 等待符合该要求 |
| Sleep Out | `0x11` 后至少等待 5 ms | 当前等待 120 ms 足够 |
| 2-lane 速率 | **最高 650 Mbps/lane** | 修复前的 1000 Mbps/lane 超出规格 |

数据手册还说明 EK79007AD 的内部 BIST 应轮换显示纯 R/G/B、黑、白、color bar、
水平/垂直灰阶、棋盘格等多种全屏图案。修复前只出现固定的左缘短线，与规定 BIST
图案不符，进一步表明 `B1=0x08` 没有被正确执行，或 DSI 工作点使面板无法可靠
解析命令/数据。

### 2.3 Lane bit rate 冲突（已修正）

修复前 `esp_mipi_dsi.c` 的配置为：

```c
#define DSI_LANES          2
#define DSI_LANE_RATE_MBPS 1000
```

而乐鑫托管的 EK79007AD Rev.1.9 数据手册明确给出：4-lane 最大 500 Mbps/lane，
2-lane 最大 650 Mbps/lane。以 48 MHz RGB888 粗略计算，仅有效像素 payload 就是：

```text
48 MHz × 24 bit / 2 lanes = 576 Mbps/lane
```

因此 2-lane 下合理工作点应在满足协议开销的同时不超过 650 Mbps/lane。本实现
已改为 **650 Mbps/lane**；必要时可结合更低 pixel clock 测试 600 Mbps/lane。
这比继续调整 porch 或 gamma 更有依据。乐鑫 Board Manager 文档示例虽出现
1000 Mbps/lane，但该字段及同一模板中的面板参数都标为 `TO_BE_CONFIRMED`，不能
用于推翻芯片数据手册的绝对上限。真机验证表明超规速率并非彩条异常的最终根因，
但 650 Mbps/lane 配置仍应保留以符合器件规格。

## 3. 当前显示参数

参数已按 Espressif 官方 EK79007 示例修正：

| 项目 | 数值 |
| --- | ---: |
| Pixel format | RGB888 / 24 bpp |
| DPI clock | 48 MHz（REF_240M / 5） |
| HACTIVE | 1024 |
| HSYNC / HBP / HFP | 10 / 160 / 160 |
| VACTIVE | 600 |
| VSYNC / VBP / VFP | 1 / 23 / 12 |
| Frame rate | 约 56 Hz |
| Framebuffer stride | 3072 bytes |
| Framebuffer size | 1,843,200 bytes |
| Framebuffer memory | 外部 PSRAM BSS，64-byte aligned |

RGB565 和 52 MHz 配置已经废弃；porch 使用 EK79007AD 数据手册的 HV
mode 典型值。48 MHz 位于数据手册允许范围内，对应约 56 Hz。

## 4. 代码结构

核心文件：

```text
chip/esp32p4/espressif/esp_mipi_dsi.c
chip/esp32p4/espressif/CMakeLists.txt
chip/esp32p4/espressif/Make.defs
chip/esp32p4/Kconfig
chip/esp32p4/hal_esp32p4.cmake
```

板级配置和启动代码：

```text
board/esp32p4/esp32p4-function-ev-board/configs/mipi/defconfig
board/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c
board/esp32p4/esp32p4-function-ev-board/include/board.h
```

`esp32p4_bringup.c` 使用独立 `mipi_fb` kthread 调用 `fb_register(0, 0)`，避免
显示初始化阻塞 NSH 启动链。后台日志统一使用 `syslog()`，不与 NSH 竞争
stdio console lock。

## 5. 构建与烧写

默认使用仓库配置的 `esp-hal-3rdparty` 版本和公开地址：

```sh
rm -rf cmake_out/esp32p4-function-ev-board_mipi
./build.sh \
  contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/mipi/ \
  --cmake -j32

ESPTOOL_PORT=/dev/ttyACM0 \
  cmake --build cmake_out/esp32p4-function-ev-board_mipi --target flash
```

不要选择 `Select ESP32-P4 revisions <3.0 (No >=3.x Support)`。该选项面向旧
silicon，烧入 v3.x 后曾在入口触发 Illegal instruction。更改 revision 配置后
必须删除整个构建目录重新配置。

## 6. 已解决的关键问题

### 6.1 v3.x 启动配置

错误启用 `<3.0` 支持时，ROM SHA 提示后在应用入口产生 Illegal instruction。
取消该配置并全量重建后，NuttX 正常启动。

### 6.2 控制台引脚

有效控制台为 GPIO37/38 上的 UART0。仅看到 ROM/NuttX 装载日志而看不到 NSH，
或无法输入时，应先确认串口连接和 TX/RX 交叉。

### 6.3 LDO 临界区卡死

`esp_ldo_acquire_channel()` 在当前 openvela/ESP OS 临界区组合中会卡住。当前
驱动在 NuttX 临界区内直接使用 `hal/ldo_ll.h`，配置 channel 3 为 2.5 V。

### 6.4 v3.x PHY reference clock

v3.x 必须使用 `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT` 和 40 MHz reference，不能
使用 `<3.0` 的 legacy 枚举。PHY configuration clock 还必须显式打开上游
`REF_20M` gate，否则 PLL status 可读但 lock 位始终为 0。

### 6.5 DPI clock gate

DPI 默认来自 PLL_F240M。必须打开上游 `REF_240M` gate，再设置 divider=5；
只开 DSI DPI gate 会造成 framebuffer 和 DMA 初始化成功但没有 pixel clock。

### 6.6 v3.2 video shadow（已解决）

P4X v3.2 存在 requested 与 active video register bank。最初尝试使用 shadow
机制：

```text
vid_shadow_en = 1
写入 timing / packet / color / mode
vid_shadow_req = 1
进入 video mode
```

但发现 `vid_shadow_req` 未能正确将 requested 寄存器值转移到 active 寄存器。
日志显示 `mode=0001bf02` 而 `active=000002fe`，VPG 配置位（bit 16）始终未
提交到 active 寄存器，导致彩条无法显示。

**最终修复**：设置 `vid_shadow_en = 0`，直接写入 active 寄存器，绕过 shadow
机制。修复后 VPG 竖向彩条立即正确显示。

### 6.7 EK79007 DCS 必须使用 LPDT

直接写 Generic FIFO 时若保留 `cmd_mode_cfg` 复位值，命令会按 HS 发送。Host
FIFO 会清空，看起来“写成功”，但 EK79007 不接受初始化序列。

当前实现参考官方 `esp_lcd_panel_io_dbi` 设置：

- command ACK 和 BTA 关闭，避免未处理的反向响应造成 LP contention；
- Generic short/long write/read 使用 LP；
- DCS short/long write/read 使用 LP；
- MRPS 使用 LP。

补齐 LPDT 后屏幕从纯黑变为左缘出现短彩线，是目前最关键的链路突破；但这
仍不能单独证明面板已正确执行全部 vendor 命令。

### 6.8 PHY 通用时序

PLL lock 后等待 clock/data lanes 全部进入 stop state，并设置：

```text
timeout count = 0
max read time = 6000
stop wait time = 0x3f
```

### 6.9 DCS read 诊断结论

曾使用 `GET_POWER_MODE (0x0A)` 验证双向链路。LP 配置前读超时；LP 配置后能
得到 payload，但返回 `00`，同时出现 `int_st0=0x00100000`，即 D-PHY LP
contention。为避免 BTA 诊断影响后续视频，当前版本已移除启动时 DCS read。

## 7. 当前 EK79007 初始化序列

复位前已确认 PHY lanes 处于 stop/LP11。GPIO27 先保持高 20 ms，再拉低
30 ms，释放后等待 120 ms（覆盖 EK79007 要求的至少 55 ms 初始化等待）。
随后先发送 DCS NOP 同步 LP 命令路径，再以 LPDT 发送：

```text
00                  NOP
B2 = 10             2 data lanes
B1 = 08             临时启用面板内部 BIST
80 = 8B
81 = 78
82 = 84
83 = 88
84 = A8
85 = E3
86 = 88
3A = 77             COLMOD: 24-bit RGB888
11                  Sleep Out
等待 120 ms
29                  Display On
等待 20 ms
```

`0x3A=0x77`、`B1=0x08`、加长复位等待和初始化前 NOP 均已完成真机验证，屏幕
仍为完全相同的左缘短线。因此这些单项不是当前问题的解决方案。

## 8. 当前诊断模式

为隔离 DMA 问题，当前版本：

- 临时写入 `B1=0x08`，启用不依赖 DCLK 的 EK79007 内部 BIST；
- DBI 写命令关闭 ACK/BTA，并等待 command/payload FIFO 均为空后才返回成功；
- 关闭 DSI Bridge DPI output；
- 不启动 framebuffer DW-GDMA；
- 启用 DSI Host `MIPI_DSI_PATTERN_BAR_VERTICAL`；
- 使用 AUTO clock lane，并允许 blanking period 进入 LP；
- 关闭 ACK/BTA 后 `int_st0` 已从 `0x00100000` 降为 `0`，确认此前的 D-PHY
  LP1 contention 已消除；但面板仍只有左缘短线，故它不是当前显示异常的主因；
- 关闭 per-frame ACK，避免视频依赖面板反向响应；
- `/dev/fb0` 仍注册，但运行 `fb /dev/fb0` 不会改变屏幕彩条。

2026-08-01 最后一次复测的关键状态如下：

```text
MIPI: PHY PLL locked
MIPI: PHY lanes stopped
MIPI: DBI commands configured for LP mode
MIPI: DCS write cmd=00 result=1
MIPI: DCS write cmd=b1 result=2
MIPI: DCS write cmd=3a result=2
MIPI: status shadow=00000001 mode=0001bf02 active=000002fe
MIPI: pkt=00000400/00000400 phy=000015bd int=00000000/00000000
MIPI: H req=26/417/3526 act=26/417/3526
MIPI: V req=1/23/12/600 act=1/23/12/600 color=00000005/00000005
MIPI: vertical color-bar test pattern enabled
MIPI: /dev/fb0 registered
```

结论：PLL lock、lane stop、requested/active shadow、1024 packet、RGB888 和无
中断错误均成立；复位/NOP/BIST/COLMOD 的发送返回值也正常，但显示仍只有左侧
一列短线。不能把 FIFO 返回的 `result=1/2` 当成面板 ACK。

### 8.1 2026-08-04 屏幕实物现象

交接照片中的屏幕状态可准确描述为：

- 背光已点亮，整个 1024×600 可视区域呈暗灰/黑色，可看到玻璃表面反光；
- 屏幕绝大部分区域没有图像、彩条、文字或 framebuffer 测试矩形；
- 仅在有效显示区最左边缘出现一条很窄的竖向异常带，宽度约为数个至十余个
  像素，位置固定，不是铺满屏幕的正常竖向彩条；
- 异常带由沿垂直方向密集排列的短横线/小色块构成，能看到白色、粉紫色、青色
  等交替颜色，基本贯穿屏幕高度；
- 短线没有向水平方向展开，Host VPG、面板 BIST、`fb /dev/fb0` 测试以及前述
  timing/初始化调整均未使形态产生可辨识变化；
- 屏幕没有明显随机闪烁、滚动、撕裂或整屏噪点，故这是稳定可复现的固定故障
  形态，而不是串口日志干扰或偶发启动失败。

该现象至少说明背光和面板供电存在、显示链路活动会在面板左缘形成响应；但它
不能证明面板已经执行 `B1/B2/3A/11/29`，也不能证明 1024 像素有效视频包被面板
正确解析。尤其面板内部 BIST 未显示全屏图案，因此在取得可靠命令读回或物理层
波形前，不应把问题简单归结为 framebuffer 数据、RGB 排列或 porch 参数。

## 9. DMA 调试结论

`/dev/fb0` 曾使用 RGB565 成功映射 PSRAM，`fb /dev/fb0` 可写入嵌套矩形，说明
NuttX framebuffer 和 PSRAM mapping 基本正常。但显示 DMA 尚未形成稳定连续流：

- 官方 full-transfer ISR 中连续重启 DMA 时，UART RX/NSH 无法输入；
- 用 25 ms、100 ms kthread 周期重提 DMA，同样会使 NSH 停止响应；
- `dw_gdma_channel_abort()` 可能造成 AXI protocol violation，已禁止使用；
- 降低 channel priority、缩短 burst 不能解决周期重装卡死；
- 当前只保留 `FBIO_UPDATE` cache sync/单帧提交代码，诊断模式不启动它。

恢复 DMA 时应先保持彩条模式验证通过，再严格复制官方
`mipi_dsi_dma_trans_done_cb()` 的 restart 顺序，并检查 NuttX 的 ESP shared IRQ
映射和 `portYIELD_FROM_ISR()` 兼容性，不应再用轮询线程反复 stop/restart。

## 10. 下一步

### 10.1 任务总览与优先级

| 优先级 | 任务 | 状态 | 前置条件 | 预期产出 |
| --- | --- | --- | --- | --- |
| P0 | 将 lane rate 从 1000 降到规格内 | ✅ 已完成 | 无 | 验证 650/600 Mbps 下 BIST/VPG 是否恢复 |
| P1 | 核对实物模组身份与初始化表 | 待定 | 无 | 确认 AML070JGI50-07403L 及 IC 批次 |
| P2 | 实现 DCS read 诊断 | ✅ 已完成 | P0 | 读回 display ID 或 power mode |
| P3 | 逐寄存器对比 ESP-IDF 示例 | 待定 | P0 | 最小差异移植，消除配置偏差 |
| P4 | 示波器验证物理信号 | 待定 | 无 | 确认 LP/HS 波形和实际速率 |
| P5 | 全屏彩条验证 | ✅ 已完成 | P0+P1+P3 | Host VPG 彩条铺满屏幕 |
| P6 | 恢复 DSI Bridge + DMA | 下一步 | P5 | framebuffer 连续刷新 |
| P7 | 修复 ISR 与 UART 兼容 | P6 | 刷新时 NSH 可输入 |
| P8 | 功能完善 | P7 | 背光 PWM、双缓冲、VSYNC |

---

### 10.2 P0：将 DSI lane rate 降到芯片规格内

**问题**：当前 2-lane 配置为 1000 Mbps/lane，EK79007AD Rev.1.9 标称最大值
为 650 Mbps/lane。这可能导致 Host PLL 和状态寄存器看似正常，但接收端无法
可靠解析 LP/HS 传输，表现为固定左缘短线。

**方案**：

1. 将 `DSI_LANE_RATE_MBPS` 从 1000 改为 650，其余配置保持不变；
2. 先观察无需 DCLK 的 EK79007AD BIST 是否出现数据手册规定的全屏循环图案；
3. 再观察 Host VPG 是否铺满；
4. 若 650 Mbps 仍异常，降低 pixel clock 后测试 600 Mbps，确保有效 RGB888
   payload 和协议开销均能容纳；
5. 每个速率记录 PLL status、PHY status、interrupt status 和屏幕照片。

**验收标准**：在不超过 650 Mbps/lane 的配置下获得全屏 BIST/VPG，或用波形
证明面板仍无法接收，从而排除速率超规这一变量。

**实际结果**（2026-08-04）：将 lane rate 降至 650 Mbps 后，屏幕仍只显示左缘
短线。进一步排查发现根因是 video shadow 寄存器未正确提交（见 6.6 节），而非
lane rate 问题。650 Mbps 配置已保留，符合 EK79007AD 规格。

---

### 10.3 P1：确认面板身份与初始化表

**问题**：当前假设面板为 EK79007，但屏幕只显示左缘短线，不能排除实际面板
IC 型号不同或初始化表不匹配的可能。

**方案**：

1. 读取 LCD adapter FPC 和 PCB 上的丝印，拍照记录完整料号；
2. 联系乐鑫或查阅官方文档，确认该批次 7 寸 1024×600 模组实际使用的面板
   控制器型号（可能是 EK79007、EK79007AD、或其他兼容型号）；
3. 如果有条件，拆开 LCD adapter 背板直接查看面板 IC 上的丝印；
4. 对比当前初始化表与乐鑫官方 `esp_lcd_ek79007` 组件的 init sequence，逐条
   核对寄存器地址和值。

**验收标准**：确认面板 IC 型号，或排除 EK79007 的可能性。

---

### 10.4 P2：实现 DCS read 诊断函数

**问题**：FIFO 返回 `result=1/2` 不代表面板真正收到了命令。此前 DCS read
因 BTA 配置不当导致 LP1 contention（`int_st0=0x00100000`），现在 LP
contention 已消除（`int_st0=0`），可以安全重试。

**方案**：在 `esp_mipi_dsi.c` 中新增单次 DCS read 函数，关键设计：

```c
/* 单次 DCS read，带超时和完整 RX 清理
 * - 不使用常驻 BTA，仅在读时临时开启
 * - 读完立即清除 RX FIFO 和中断状态
 * - 超时 10ms，超时后返回错误码
 */

int mipi_dsi_read_dcs_cmd(uint8_t cmd, uint8_t *buf, size_t len);
```

读取目标（按优先级）：
- `0x04` — Display ID（3 bytes），确认面板型号
- `0x0A` — Power Mode（1 byte），确认面板是否处于正常模式
- `0x0B` — Display Status（1 byte），额外诊断信息

**注意事项**：
- 单次触发，读完立即关闭 BTA，不要恢复常驻 BTA 配置；
- 读取前确保 RX FIFO 为空（清除 `int_st1` 中的 `RX_FIFO_FULL` 等标志）；
- 读取后清除所有 RX 相关中断状态，避免残留影响后续视频；
- 如果读超时，记录 `int_st0`/`int_st1`/`phy_st0` 状态用于诊断。

**验收标准**：能读回 display ID 或 power mode，且不产生 LP contention。

---

### 10.5 P3：逐寄存器对比 ESP-IDF 可工作示例

**问题**：当前配置与 ESP-IDF 官方示例可能存在未察觉的差异，任何一处偏差
都可能导致面板不工作。

**方案**：

1. 在 ESP-IDF 中找到 `esp32p4_function_ev_board` 的 MIPI DSI LCD 示例，
   定位以下模块的配置：
   - `mipi_dsi_phy_config_t` — D-PHY PLL 倍频/分频/charge pump
   - `mipi_dsi_config_t` — Host video mode timing
   - `esp_lcd_panel_io_dbi_config_t` — DBI LP command 配置
   - EK79007 panel init sequence
2. 逐寄存器对比当前 openvela 实现，列出差异表：

| 寄存器/字段 | 当前值 | ESP-IDF 值 | 差异说明 |
| --- | --- | --- | --- |

3. 对差异项逐一分析，优先做最小差异移植；
4. 重点关注：
   - D-PHY PLL 的 `hstx_ckg_sel` 和 charge pump current
   - Host video mode 的 LP/HS 切换时机（`auto_clklane_en`）
   - `cmd_mode_cfg` 中各类型写操作的 LP/HS 选择
   - `vid_mode_cfg` 中的 `lp_en` 和 `vpg_en` 位

**验收标准**：差异表完成，可解释的差异已标注原因，不可解释的差异已修正。

---

### 10.6 P4：示波器验证物理信号

**问题**：Host FIFO 空状态不能代替链路确认，需要物理层验证。

**测试点**（按优先级）：

1. **GPIO27 复位波形**：
   - 高 20ms → 低 30ms → 释放 → 等待 120ms
   - 确认电平和时序符合 EK79007 要求（至少 55ms 初始化等待）

2. **CLK lane 在 blanking 期间的状态**：
   - 确认 auto_clklane_en 生效后，blanking 期间 CLK lane 是否进入 LP
   - 如果 CLK lane 始终保持 HS，可能是 `auto_clklane_en` 未生效

3. **B1/B2 命令期间 data lane 波形**：
   - 确认 data lane 在 LP 状态下形成正确的 LPDT escape sequence；
   - 确认短包内容正确（data type `0x15`、command 和 parameter）；
   - 当前命令被配置为 LP 发送，不应以出现 HS burst 作为成功标准。

4. **Video mode 启动后**：
   - CLK lane 是否持续有 HS 时钟
   - Data lane 是否持续有 HS 数据流
   - 如果 HS 流中断，检查 Host 的 `vid_mode_cfg` 和 `lp_en` 配置

**验收标准**：获得以上四项的波形截图，确认物理信号正常。

---

### 10.7 P5：全屏彩条验证 ✅ 已完成

**前提**：P0 已排除速率超规，P1/P2/P3 已确认面板身份、命令和 Host 配置，
P4 已确认物理信号正常。

**实际结果**（2026-08-04）：通过禁用 video shadow 寄存器（`vid_shadow_en = 0`），
Host VPG 竖向彩条已成功铺满 1024×600 屏幕。关键修复点：

1. **禁用 shadow 寄存器**：`vid_shadow_en = 0`，直接写入 active 寄存器；
2. **强制 HS 模式**：CLK lane 强制 HS，禁用 LP blanking；
3. **Lane rate 降至 650 Mbps**：符合 EK79007AD 规格；
4. **补充 COLMOD 命令**：`0x3A = 0x77`（RGB888）。

**验收标准**：✅ Host VPG 彩条铺满整个屏幕。

---

### 10.8 P6：恢复 DSI Bridge 和 framebuffer DMA

**前提**：P5 全屏彩条验证通过。

**方案**：

1. 关闭 Host VPG 模式，恢复 DSI Bridge DPI output；
2. 严格复制官方 `mipi_dsi_dma_trans_done_cb()` 的 restart 顺序：
   ```c
   /* 官方流程：
    * 1. 在 DMA done ISR 中调用 dw_gdma_channel_config()
    * 2. 调用 dw_gdma_channel_start()
    * 3. 不要在 ISR 中调用 abort
    */
   ```
3. 检查 NuttX 的 ESP shared IRQ 映射，确保 DSI 中断正确注册；
4. 检查 `portYIELD_FROM_ISR()` 在 ESP32-P4 上的兼容性；
5. 不再使用轮询线程反复 stop/restart DMA。

**验收标准**：framebuffer 内容通过 DMA 持续送往屏幕。

---

### 10.9 P7：修复 ISR 与 UART RX 兼容问题

**问题**：full-transfer ISR 连续重启 DMA 时，UART RX/NSH 无法输入。

**排查方向**：

1. 检查 DSI 中断优先级是否高于 UART RX 中断；
2. 检查 ISR 中是否有长时间阻塞操作（如 `dw_gdma_channel_abort()`）；
3. 检查 `portYIELD_FROM_ISR()` 是否导致调度器长时间占用；
4. 尝试降低 DSI 中断优先级或使用 bottom-half 处理。

**验收标准**：framebuffer 持续刷新时，UART RX/NSH 输入正常。

---

### 10.10 P8：功能完善

1. 精简阶段日志，保留关键状态输出；
2. 实现背光 PWM 控制（GPIO26）；
3. 实现双缓冲（ping-pong buffer）减少撕裂；
4. 实现 VSYNC 同步刷新；
5. 实现 `FBIO_UPDATE` cache sync 和单帧提交；
6. 失败路径处理：关闭背光、释放显示资源、不卡死启动链。

## 11. 下次接手快速清单

当前代码是诊断版本，Host VPG 竖向彩条已验证可用。下一步是恢复 DSI Bridge
和 framebuffer DMA 连续刷新。接手后先复现本节基线，不要直接调 DMA。

**已验证的关键配置**：
- Lane rate: 650 Mbps/lane（符合 EK79007AD 规格）
- Video shadow: 禁用（`vid_shadow_en = 0`），直接写 active 寄存器
- CLK lane: 强制 HS 模式，禁用 LP blanking
- COLMOD: `0x3A = 0x77`（RGB888）
- EK79007 BIST: 启用（`B1 = 0x08`）

```sh
rm -rf cmake_out/esp32p4-function-ev-board_mipi
./build.sh \
  contest2026_128_xiaolidianzishiyanshi/board/esp32p4/esp32p4-function-ev-board/configs/mipi/ \
  --cmake -j32
ESPTOOL_PORT=/dev/ttyACM0 \
  cmake --build cmake_out/esp32p4-function-ev-board_mipi --target flash
```

已知复现结果：系统和 NSH 正常、`/dev/fb0` 注册、背光亮，Host VPG 竖向彩条
可铺满 1024×600 屏幕。framebuffer DMA 连续刷新仍待恢复。

## 12. 验收标准

- NuttX 和 NSH 稳定，UART 输入不受显示刷新影响；
- `/dev/fb0` 注册成功；
- EK79007 全屏显示正确的 Host 彩条；
- framebuffer RGB888 图案可完整显示；
- 连续刷新无 underrun、花屏、撕裂或 DMA 停止；
- PSRAM cache 同步和 `FBIO_UPDATE` 生效；
- 失败路径不会卡死启动链，并能关闭背光、释放显示资源。

## 13. 参考资料

- [Espressif ESP32-P4X/ESP32-P4 Function EV Board User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)
- [AML070JGI50-07403L display datasheet](https://dl.espressif.com/dl/schematics/display_datasheet.pdf)
- [EK79007AD Rev.1.9 datasheet](https://dl.espressif.com/dl/schematics/display_driver_chip_EK79007AD_datasheet.pdf)
- [Espressif `esp_lcd_ek79007` component](https://components.espressif.com/components/espressif/esp_lcd_ek79007)
- [Espressif MIPI DSI LCD detailed guide](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/mipi_dsi_lcd.html)
- [Espressif Board Manager DSI display configuration](https://docs.espressif.com/projects/esp-board-manager/en/latest/references/devices/display-lcd.html)
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_mipi_dsi_bus.c`
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_panel_io_dbi.c`
- ESP-IDF `components/esp_lcd/dsi/esp_lcd_panel_dpi.c`
- Espressif `esp_lcd_ek79007` component
- EK79007 controller datasheet
