# hello_app（应用形态样例）

映射到 openvela `packages/demos/contest2026_000_hello_app`。
队伍把应用代码放在本目录下。

## 板级测试程序

- `lcd_test [lcd-device]`：通过 NuttX LCD 字符设备绘制测试图。
- `touch_test [input-device] [sample-count]`：默认读取
  `/dev/input0`，打印 touchscreen ioctl 支持情况和每个触点的事件、坐标。
  `sample-count` 省略或为 `0` 时持续运行，可用 Ctrl-C 结束。
- `ppa_test [all|fill|blend|srm|cache|invalid|lifecycle|stress|perf]`：
  对 ESP32-P4 PPA 输出做 CPU reference 逐字节比较、DMA guard 检查、错误
  恢复、压力和性能测试。覆盖 Fill、Blend、SRM、PSRAM、四角度旋转、镜像、
  双线性缩放及 alpha 更新模式；测试只通过 NuttX `esp32p4_ppa_*` 接口
  驱动 HAL 数据通路，不依赖 ESP-IDF driver/LL 公共类型；不带参数时运行
  完整功能矩阵。
- `lvgl_ppa`：初始化 NuttX LVGL 9.1 LCD/触摸驱动，在 `lv_init()` 之后通过
  `lv_draw_create_unit()` 注册应用侧 PPA draw unit，并显示由 PPA Fill 与软件
  渲染共同完成的测试界面。适配层不修改 LVGL 源码；不支持的任务自动回退
  到 LVGL 软件渲染。硬件路径支持位于映射 Flash、内部 RAM 或 PSRAM 的
  RGB565/RGB888/XRGB8888/ARGB8888/A8 变量图像，支持格式转换、全局透明度、
  像素 Alpha、A8 着色、色键、1/16 精度的横纵缩放及 90 度倍数旋转。
  Blend 使用互不重叠的前景、背景和输出缓冲区，符合 ESP32-P4 PPA 的 DMA
  约束。任意角度旋转、普通 RGB 重着色、位图遮罩、圆角裁剪、平铺和斜切
  仍由 LVGL 软件渲染。
- `lvgl_widget`：以 OpenVela `lvgldemo` 为启动模板，固定运行 LVGL widgets
  demo，并在创建界面之前注册同一个 PPA draw unit。LCD 使用 `/dev/lcd0`，
  启用触摸后输入使用 `/dev/input0`。该命令仅在 `LV_USE_DEMO_WIDGETS` 已启用
  时生成，任务栈为 32768 字节。

构建 `ppa_test` 需要同时启用 `CONFIG_ESP32P4_PPA` 和
`CONFIG_LVX_USE_DEMO_CONTEST2026_128_PPA_TEST`。

构建 `lvgl_ppa`（以及 widgets 已启用时的 `lvgl_widget`）还需要启用
`CONFIG_LVX_USE_DEMO_CONTEST2026_128_LVGL_PPA`、`CONFIG_GRAPHICS_LVGL`、
`CONFIG_LV_USE_NUTTX_LCD`。若 LVGL 目标缓冲区不满足 PPA 的 cache-line
对齐约束，适配层会使用对齐的中间缓冲区，不要求修改 LVGL 源码或全局
draw-buffer 配置。
