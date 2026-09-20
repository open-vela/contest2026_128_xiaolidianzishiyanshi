# ESP32P4 OpenVela BSP适配

## 一、作品简介
将NuttX上游BSP Backport到OpenVela，在上游基础上添加SDMMC、ESP-Hosted（WiFi）、DSI适配。

## 二、选题方向
新硬件适配

## 三、目录结构

- `app/hello_app/`                            — 开发阶段用到的一些单元测试用例
- `app/xiaozhi/`                              — “小智AI”示例程序
- `board/esp32p4/esp32p4-function-ev-board/`  — ESP32P4核心板-板级适配
- `chip/esp32p4`                              — ESP32P4-芯片级适配
- `logs/`                                     — AI Coding 日志
- `docs/`                                     — 开发过程文档
- `skills/`                                   — AI Skill

## 四、运行方式

配置：
```bash
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/xiaozhi/ --cmake menuconfig
```

注意：本仓库使用的是`https://github.com/lxydiy/esp-hal-3rdparty`仓库的`master.c-backport-to-openvela`分支，相比`master.c`新增3个补丁。

编译：
```bash
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/xiaozhi/ --cmake -j$(nproc)
```

烧录：
```bash
export PATH=$(pwd)/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/:$PATH
ESPTOOL_PORT=/dev/ttyACM0 cmake --build cmake_out/esp32p4-function-ev-board_xiaozhi --target flash
```

```bash
minicom -D/dev/ttyACM0 -b115200
```
**USB JTAG串口未适配，请使用外接USB转串口**，板上GPIO37接RXD、GPIO38接TXD。

运行`xiaozhi`可启动BSP示例程序。

详细构建教程请参考：`contest2026_128_xiaolidianzishiyanshi/docs/dev/How-to-build.md`

## 五、AI Coding 使用说明
<说明本作品如何借助 AI 辅助开发：
- 在需求拆解 / 方案设计 / 编码 / 调试 / 文档等环节如何与 AI 协作；
- AI 对开发效率或质量带来的实际帮助。
完整对话日志见 logs/ 目录>
