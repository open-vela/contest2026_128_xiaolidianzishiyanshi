# EMAC DMA (Ethernet) 调试记录

寄存器基地址：`0x50099000`
寄存器描述：`cmake_out/esp32p4-function-ev-board_python/arch/risc-v/src/common/espressif/esp-hal-3rdparty/components/soc/esp32p4/register/hw_ver1/soc/emac_dma_struct.h`

结论：部分开发板没有以太网PHY，启用相关CONFIG会导致PHY复位失败，在后续初始化Socket时Panic。
