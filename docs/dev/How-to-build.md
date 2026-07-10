# 编译方法

配置：
```bash
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake menuconfig
```

编译：
```bash
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j$(nproc)
```

如果改了CMakeLists.txt，可能需要重新编译：
```bash
rm -rf cmake_out/esp32p4-function-ev-board_nsh/ && ./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j$(nproc)
```