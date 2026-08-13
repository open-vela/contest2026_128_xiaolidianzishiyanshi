# ##############################################################################
# arch/risc-v/src/common/espressif/esp_hosted/esp_hosted.cmake
#
# Source file list for ESP-Hosted, included by CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

set(ESP_HOSTED_SRCS)

# ##############################################################################
# Port layer (NuttX adaptation, this directory)
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_port.c
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_netdev.c
  ${CMAKE_CURRENT_LIST_DIR}/esp_netif_stub.c
)

# ##############################################################################
# Transport adaptation (this directory, selected by config)
# ##############################################################################

if(CONFIG_ESP_HOSTED_SDIO)
  list(APPEND ESP_HOSTED_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_transport_sdio.c)
elseif(CONFIG_ESP_HOSTED_SPI)
  list(APPEND ESP_HOSTED_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_transport_spi.c)
elseif(CONFIG_ESP_HOSTED_UART)
  list(APPEND ESP_HOSTED_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/esp_hosted_transport_uart.c)
endif()

# ##############################################################################
# ESP-Hosted upstream: Transport core
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${ESP_HOSTED_ROOT}/host/drivers/transport/transport_drv.c
  ${ESP_HOSTED_ROOT}/host/drivers/transport/transport_util.c
  ${ESP_HOSTED_ROOT}/host/drivers/serial/serial_drv.c
  ${ESP_HOSTED_ROOT}/host/drivers/serial/serial_ll_if.c
)

# ##############################################################################
# ESP-Hosted upstream: RPC core
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core/rpc_core.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core/rpc_req.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core/rpc_rsp.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core/rpc_evt.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core/rpc_utils.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/slaveif/rpc_slave_if.c
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/wrap/rpc_wrap.c
)

# ##############################################################################
# ESP-Hosted upstream: Virtual Serial
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${ESP_HOSTED_ROOT}/host/drivers/virtual_serial_if/serial_if.c
)

# ##############################################################################
# ESP-Hosted upstream: API layer
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${ESP_HOSTED_ROOT}/host/api/src/esp_hosted_api.c
  ${ESP_HOSTED_ROOT}/host/api/src/esp_hosted_transport_config.c
)

# ##############################################################################
# ESP-Hosted upstream: Common code
# ##############################################################################

list(APPEND ESP_HOSTED_SRCS
  ${ESP_HOSTED_ROOT}/common/protobuf-c/protobuf-c/protobuf-c.c
  ${ESP_HOSTED_ROOT}/common/proto/esp_hosted_rpc.pb-c.c
)

if(CONFIG_ESP_HOSTED_USE_MEMPOOL)
  list(APPEND ESP_HOSTED_SRCS
    ${ESP_HOSTED_ROOT}/common/mempool/mempool.c
    ${ESP_HOSTED_ROOT}/common/mempool/mempool_ll.c
  )
endif()

# ##############################################################################
# Bluetooth (optional)
# ##############################################################################

if(CONFIG_ESP_HOSTED_BT)
  list(APPEND ESP_HOSTED_SRCS
    ${ESP_HOSTED_ROOT}/host/drivers/bt/vhci_drv.c)
else()
  list(APPEND ESP_HOSTED_SRCS
    ${ESP_HOSTED_ROOT}/host/drivers/bt/hci_stub_drv.c)
endif()

# ##############################################################################
# Files NOT compiled from upstream:
# - host/api/src/esp_wifi_weak.c  (depends on ESP-IDF Wi-Fi Remote, not needed)
# - host/port/esp/freertos/       (ESP-IDF port layer, replaced by this directory)
# - host/api/src/esp_hosted_ota_api.c (optional, can be added later)
# ##############################################################################

target_sources(arch PRIVATE ${ESP_HOSTED_SRCS})
