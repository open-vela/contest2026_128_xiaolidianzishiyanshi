# ESP32P4 v1.x和v3.x版本对于Nuttx启动的影响

在clone主线Nuttx系统（release v13.0），编译后烧录到不同的系统，发现以下现象：

## 现象

### 正常启动

#### v1.x
```log
ESP-ROM:esp32p4-eco2-20240710
Build:Jul 10 2024
rst:0x1 (POWERON),boot:0x20f (SPI_FAST_FLASH_BOOT)
SPI mode:DIO, clock div:1
load:0x30100000,len:0x44
load:0x4ff00000,len:0x5390
load:0x4ff05400,len:0x1160
SHA-256 comparison failed:
Calculated: 259ef20e1e24593e291ab65d203fd27389c2d45f23dd37f02ca1a403d3bfa472
Expected: 00000000a07a0000000000000000000000000000000000000000000000000000
Attempting to boot anyway...
entry 0x4ff05074
*** Booting NuttX ***
tcm: lma 0x00002020 vma 0x30100000 len 0x44     (68)
dram: lma 0x0000206c vma 0x4ff00000 len 0x5390   (21392)
dram: lma 0x00007404 vma 0x4ff05400 len 0x1160   (4448)
padd: lma 0x00008578 vma 0x00000000 len 0x7aa0   (31392)
imap: lma 0x00010020 vma 0x40030020 len 0xb030   (45104)
padd: lma 0x0001b058 vma 0x00000000 len 0x4fa0   (20384)
imap: lma 0x00020000 vma 0x40000000 len 0x225d4  (140756)
total segments stored 7
WARNING: NuttX supports ESP32-P4 chip revision > v3.0 (chip revision is v1.3).
Ignoring this error and continuing because `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` is set...
THIS MAY NOT WORK! DON'T USE THIS CHIP IN PRODUCTION!

NuttShell (NSH) NuttX-13.0.0
nsh> help
help usage:  help [-v] [<cmd>]

    .           cp          expr        mount       set         truncate    
    [           cmp         false       mv          kill        uname       
    ?           dirname     fdinfo      pidof       pkill       umount      
    alias       df          free        printf      sleep       unset       
    unalias     dmesg       help        ps          usleep      uptime      
    basename    echo        hexdump     pwd         source      watch       
    break       env         ls          reboot      test        xd          
    cat         exec        mkdir       rm          time        wait        
    cd          exit        mkrd        rmdir       true        

Builtin Apps:
    dd           dumpstack    getprime     nsh          ostest       sh           
nsh> 
```
#### v3.x

v3.x除了没有提示版本不支持以外，其它日志和v1.x相同。

### v3.x文件烧录到v1.x
无限重启，APP无日志输出

相关日志：
```log
ESP-ROM:esp32p4-eco2-20240710
Build:Jul 10 2024
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0x20f (SPI_FAST_FLASH_BOOT)
SPI mode:DIO, clock div:1
load:0x30100000,len:0x44
load:0x4ff40000,len:0x5610
load:0x4ff45680,len:0x10e0
SHA-256 comparison failed:
Calculated: c9dc38e4c97d80c535263c3c8b9a59ef158404b2eae4dc43faffa3f41db149c5
Expected: 00000000a0780000000000000000000000000000000000000000000000000000
Attempting to boot anyway...
entry 0x4ff452f4
ESP-ROM:esp32p4-eco2-20240710
Build:Jul 10 2024
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0x20f (SPI_FAST_FLASH_BOOT)
SPI mode:DIO, clock div:1
load:0x30100000,len:0x44
load:0x4ff40000,len:0x5610
load:0x4ff45680,len:0x10e0
SHA-256 comparison failed:
Calculated: c9dc38e4c97d80c535263c3c8b9a59ef158404b2eae4dc43faffa3f41db149c5
Expected: 00000000a0780000000000000000000000000000000000000000000000000000
Attempting to boot anyway...
entry 0x4ff452f4
```
会在entry 0x4ff452f4处停顿0.5s左右。可能是__start时跑飞，在这种状态下，0.5s自动由WDT复位

### v1.x文件烧录到v3.x
报错非法指令位于`PC      : 0x4ff05074`，反查后可知，此地址对应`__start`标号地址。

非法指令日志与`addr2line`：
```log
ESP-ROM:esp32p4-eco7-20260109
Build:Jan  9 2026
rst:0x7 (HP_SYS_HP_WDT_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)
SPI mode:DIO, clock div:1
load:0x30100000,len:0x44
load:0x4ff00000,len:0x5390
load:0x4ff05400,len:0x1160
SHA-256 comparison failed:
Calculated: e7776dee63df449da54c8d2a5cdeb363bcc2b0e8db102b6a4bf06d901e393e69
Expected: 00000000a07a0000000000000000000000000000000000000000000000000000
Attempting to boot anyway...
entry 0x4ff05074
Guru Meditation Error: Core 0 panic'ed (Illegal instruction)
Core 0 register dump:
PC      : 0x4ff05074  RA      : 0x4fc055be  SP      : 0x4ffbcdf0  GP      : 0x00000000
TP      : 0x00000000  T0      : 0x4fc0a062  T1      : 0x20000000  T2      : 0x4ffbce28
S0      : 0x00000000  S1      : 0x0000003d  A0      : 0x00000011  A1      : 0x0000000a
A2      : 0x00000000  A3      : 0x4ffbfea4  A4      : 0x00000001  A5      : 0x4ff05074
A6      : 0x80000000  A7      : 0x00000010  S2      : 0x00000000  S3      : 0x4ffc0000
S4      : 0x0000ffff  S5      : 0x4fc1ddb4  S6      : 0x00008564  S7      : 0x4ffc0000
S8      : 0x00000000  S9      : 0x00000000  S10     : 0x00000000  S11     : 0x00000000
T3      : 0x00000000  T4      : 0x65726620  T5      : 0x55504320  T6      : 0x64696c61
MSTATUS : 0x00001881  MCAUSE  : 0x38000002  MTVAL   : 0x00000000  INTLEVEL: 0x00000010


Stack memory:
4ffbcdf0: 0x3ff53685 0x15e9ca86 0xeefd327e 0x43c36591 0x00000000 0x00010000 0x2f0203e9 0x4ff05074
4ffbce10: 0x4ff05400 0x00001160 0x000000ee 0x00000012 0x00ffff00 0x00000000 0x40008570 0x7533885e
4ffbce30: 0xee6d77e7 0x9d44df63 0x2a8d4ca5 0x63b3de5c 0xe8b0c2bc 0x6a2b10db 0x906df04b 0x693e391e
4ffbce50: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000
4ffbce70: 0x4ffbcf90 0x00000101 0x00000002 0xee6d77e7 0x9d44df63 0x2a8d4ca5 0x63b3de5c 0xe8b0c2bc
4ffbce90: 0x6a2b10db 0x906df04b 0x693e391e 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000
4ffbceb0: 0x00000000 0x00000000 0x00000000 0x3a297272 0x766e6920 0x64696c61 0x55504320 0x65726620
4ffbced0: 0x6e657571 0x76207963 0x65756c61 0x00000000 0x00000000 0x00000000 0x3d000000 0x00000080
4ffbcef0: 0x00000000 0x00000000 0x802b0300 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000
4ffbcf10: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000
4ffbcf30: 0x00000000 0x00000000 0x00000000 0x00032c00 0x00000000 0x00000000 0x00000000 0x109c4629
4ffbcf50: 0x00000000 0x4ffbfff8 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000
4ffbcf70: 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x4ffbfff8 0x4ffbf008 0x4fc02b7c
4ffbcf90: 0x5dd6f21a 0x0c89cab6 0x4fc1d6f0 0x4fc1d440 0x009b3a50 0x901e2fd6 0x90ee0598 0x4ffbcfc0
4ffbcfb0: 0x00000000 0x00000000 0x00000000 0x00000000 0x7486b604 0x143639e9 0xefa8a13c 0x5e348ac4
4ffbcfd0: 0xf018dd4e 0x814d123b 0x805bd836 0x41f92d9c 0x3c30122d 0xc27ea750 0xa13a5e4c 0x261540c0
4ffbcff0: 0x52e53375 0x91bb7a68 0x428066bb 0x24787f93 0x3881ec41 0x001d6d35 0x7b33e05f 0x6934ddff
4ffbd010: 0x3b04b20a 0x83300334 0x0b81cb59 0xedc85fa0 0xfe7cad87 0xa0e54841 0x74974172 0x775aca64
4ffbd030: 0xdbf4118c 0x63d9295f 0x79e95dbc 0xb4ce62f1 0x99ced351 0x8297b72c 0x2a6b80b9 0x3bb2d6a9
4ffbd050: 0x1ceb61bc 0x25dc46c3 0x4f4a360c 0x2dffd2a6 0x30462386 0xcab8a3cf 0x45c4aa0b 0x33d560bb
4ffbd070: 0x8e4d13ec 0x381e1689 0x2dbf6def 0x61857648 0xd57e87bf 0x164efe60 0x8c899c51 0xc3d5a8aa
4ffbd090: 0x37091c66 0x409c066c 0x1dfcfb74 0x8d0f7e1c 0x31b90fff 0x8d334007 0x5733ef62 0xd1dffb39
4ffbd0b0: 0xab8a4413 0x18b0899f 0xc763cafe 0x67e0ec03 0xf7c8ae0d 0x93b7bd41 0x8f9b28d0 0xe69098c9
4ffbd0d0: 0x7000bf73 0xcfd64782 0x21815949 0xaae8bfc7 0x46b1bb38 0xdc004932 0x91034b29 0xabb5e095
4ffbd0f0: 0x7708d61f 0x0d8ed4e9 0x680c6468 0xb2df7c03 0xcc48182c 0xa125055a 0x30a1a956 0xba40185a
4ffbd110: 0xb290787a 0xbdd2c5ba 0xdf123777 0x9b904e15 0x6ddc5018 0x5feeec2e 0x588a8963 0x65f8ab1b
4ffbd130: 0x96a7b58a 0xb68c05ad 0xaa1b3fa5 0x230c74e3 0xff318d0c 0x152f2ab5 0x0405d378 0x0248f7fe
4ffbd150: 0xb67a4277 0x8d1c52cc 0xd091bbd2 0x0838db2d 0xc36e3685 0xfb2a7615 0x2fa5399c 0x22e1d9f9
4ffbd170: 0x9ea6ddb5 0xfffa0672 0x19bb33ef 0xedefa0d6 0x28f96fb6 0x6b9d2dbf 0xe4eb7a43 0x06741be3
4ffbd190: 0xe38b6db6 0x67e36a7a 0x0e9896d0 0xb29a2255 0x30031b88 0x9a5c118d 0xe3672b59 0x6019163a
4ffbd1b0: 0x7a33987b 0x8d1a11b2 0xad2890a3 0xbae1bb0e 0x967ee9f2 0xd4ea61de 0x9389e562 0xb5fb3c88
4ffbd1d0: 0x83134b4d 0x5d9e2b10 0x15f7d503 0xf41a928f 0x72acee00 0x79368cb3 0xb6b99dd7 0x31380f58
```

```bash
$ riscv-none-elf-addr2line -e ./nuttx -f -C 0x4ff05074
__start
??:?
```

## 分析
通过全局搜索`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`，发现有以下区别：

*待补充*

