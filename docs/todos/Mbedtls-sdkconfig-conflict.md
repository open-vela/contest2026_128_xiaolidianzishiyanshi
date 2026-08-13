# mbedTLS库和ESP-HAL-3rdParty冲突问题

OpenVela使用的mbedTLS库可能和ESP-HAL使用的库产生了冲突。具体表现为：在启用python配置，
并打开javascript解释器后，产生了链接报错。

分析原因可知：
`cmake_out/esp32p4-function-ev-board_python/arch/risc-v/src/common/espressif/esp-hal-3rdparty/nuttx/esp32p4/include/sdkconfig.h`
750行左右，取消定义了许多mbedTLS配置宏。取消原因未知，但这些宏定义影响到了OpenVela侧
对应库的编译，进而导致链接器找不到符号问题。

注释这些`#undef`可以避免报错，但真正解决问题需要分析出取消这些定义的真正原因，并给出非Workaround的解决方案。

可能的原因是：
- OpenVela的app在ESP32-P4平台使用的是esp-hal-3rdparty内置的mbedtls库，但hal没有考虑
app实际用到的功能，没有包含对应宏定义配置。
