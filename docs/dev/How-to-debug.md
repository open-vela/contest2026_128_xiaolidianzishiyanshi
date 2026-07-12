# 调试指南

## 静态分析

### 设置工具链到path

```bash
export PATH=/home/lxy/openvela/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/:$PATH
```

### 根据地址查询代码行

```bash
riscv-none-elf-addr2line  -e ./nuttx -f -C 0x400172ba
```

## 动态调试

### 启动OpenOCD

```bash
openocd -f board/esp32p4-builtin.cfg -c init -c "reset halt" -c "esp appimage_offset 0x2000"
```
注意：SimpleBoot模式下需要指定App地址到0x2000

### VSCode配置

`.vscode/launch.json`
```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "gdb",
            "request": "attach",
            "name": "Debug Microcontroller",
            "target": "extended-remote :3333",
            "executable": "${workspaceFolder}/cmake_out/esp32p4-function-ev-board_python/nuttx",
            "cwd": "${workspaceRoot}/nuttx",
            "gdbpath": "/usr/bin/gdb-multiarch",
            "autorun": [
                "set remotetimeout 40",
                "mon reset halt",
                "maintenance flush register-cache"
            ]
        }

    ]
}
```
### 编译配置

需要至少启用下列选项
```
CONFIG_DEBUG_INFO=y
CONFIG_DEBUG_SYMBOLS=y
CONFIG_FRAME_POINTER=y
```
