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

烧录：
```bash
ESPTOOL_PORT=/dev/ttyACM0 cmake --build cmake_out/esp32p4-function-ev-board_nsh --target flash
```

## 指定本地esp-hal-3rdpary

由于每次清空CMake工程都要从GitHub拉取3rdparty，为了避免此类情况，可使用下面的命令改用本地仓库。
```bash
export ESP_HAL_3RDPARTY_URL=http://192.168.8.142:3000/espressif/esp-hal-3rdparty.git
export ESP_HAL_3RDPARTY_VERSION=master.c-backport-to-openvela
```
之后运行上述编译命令即可。

## 给esp-hal-3rdpary添加Patch
有时需要对esp-hal-3rdparty进行backport，或添加更多功能。通过上节所述方法搭建本地仓库后，可自行创建分支对3rdparty进行修改。

首先在其它目录（比如home目录）clone esp-hal-3rdparty，并创建相应开发分支。注意不要在cmake_out下的仓库中开发。
```bash
git clone http://192.168.8.142:3000/espressif/esp-hal-3rdparty.git
cd esp-hal-3rdparty
git switch release/master.c  # release/master.c是位于远程的分支名，将基于这一分支进行开发
git checkout -b master.c-backport-to-openvela
```

之后上传分支到远程仓库（假设origin）
```bash
git push -u origin master.c-backport-to-openvela
```

创建分支后，回到cmake在配置阶段clone的git仓库，使用命令切换到对应分支：
```bash
cd cmake_out/esp32p4-function-ev-board_nsh/arch/risc-v/src/common/espressif/esp-hal-3rdparty
git switch -C master.c-backport-to-openvela
git pull
```

随后回到repo根目录，按正常流程编译工程。
