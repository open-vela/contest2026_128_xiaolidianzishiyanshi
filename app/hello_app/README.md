# hello_app（应用形态样例）

映射到 openvela `packages/demos/contest2026_000_hello_app`。
队伍把应用代码放在本目录下。

## 板级测试程序

- `lcd_test [lcd-device]`：通过 NuttX LCD 字符设备绘制测试图。
- `touch_test [input-device] [sample-count]`：默认读取
  `/dev/input0`，打印 touchscreen ioctl 支持情况和每个触点的事件、坐标。
  `sample-count` 省略或为 `0` 时持续运行，可用 Ctrl-C 结束。
