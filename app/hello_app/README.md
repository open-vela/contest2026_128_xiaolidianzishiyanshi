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

构建 `ppa_test` 需要同时启用 `CONFIG_ESP32P4_PPA` 和
`CONFIG_LVX_USE_DEMO_CONTEST2026_128_PPA_TEST`。
