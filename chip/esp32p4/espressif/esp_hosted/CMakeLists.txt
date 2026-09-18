# ##############################################################################
# arch/risc-v/src/common/espressif/esp_hosted/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(NOT CONFIG_ESP_HOSTED)
  return()
endif()

# Fetch only the portable ESP-Hosted sources.  The upstream ESP-IDF component
# CMake file is intentionally not evaluated; esp_hosted.cmake below selects
# the sources that use the NuttX OS and bus adaptation layers.

set(ESP_HOSTED_REPO_NAME esp-hosted-mcu)
set(ESP_HOSTED_REPO_DIR
    "${CMAKE_BINARY_DIR}/arch/risc-v/src/common/espressif/${ESP_HOSTED_REPO_NAME}")

if(DEFINED ENV{ESP_HOSTED_URL})
  set(ESP_HOSTED_URL $ENV{ESP_HOSTED_URL})
else()
  set(ESP_HOSTED_URL "https://github.com/espressif/esp-hosted-mcu.git")
endif()

if(DEFINED ENV{ESP_HOSTED_VERSION})
  set(ESP_HOSTED_VERSION $ENV{ESP_HOSTED_VERSION})
else()
  set(ESP_HOSTED_VERSION "4d9dc3857f9b720a43f553326dd029132b587b73")
endif()

if(NOT IS_DIRECTORY "${ESP_HOSTED_REPO_DIR}")
  FetchContent_Declare(
    esp_hosted_mcu
    GIT_REPOSITORY ${ESP_HOSTED_URL}
    GIT_TAG ${ESP_HOSTED_VERSION}
    GIT_SUBMODULES ""
    SOURCE_DIR ${ESP_HOSTED_REPO_DIR})
  FetchContent_GetProperties(esp_hosted_mcu)
  if(NOT esp_hosted_mcu_POPULATED)
    FetchContent_Populate(esp_hosted_mcu)
  endif()
endif()

set(ESP_HOSTED_ROOT ${ESP_HOSTED_REPO_DIR})
set(ESP_HOSTED_PROTOBUF_ROOT ${NUTTX_DIR}/../external/protobuf-c/protobuf-c)

# Keep the fetched ESP-Hosted checkout pristine.  Apply NuttX-specific fixes
# to a build-directory copy so a clean configure reproduces the exact source
# without depending on mutations left in FetchContent's checkout.

set(ESP_HOSTED_SDIO_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/0001-sdio-streaming-rx-backpressure.patch")
set(ESP_HOSTED_PATCHED_ROOT "${CMAKE_CURRENT_BINARY_DIR}/patched")
set(ESP_HOSTED_SDIO_PATCHED_SOURCE
    "${ESP_HOSTED_PATCHED_ROOT}/host/drivers/transport/sdio/sdio_drv.c")

file(MAKE_DIRECTORY
     "${ESP_HOSTED_PATCHED_ROOT}/host/drivers/transport/sdio")
configure_file(
  "${ESP_HOSTED_ROOT}/host/drivers/transport/sdio/sdio_drv.c"
  "${ESP_HOSTED_SDIO_PATCHED_SOURCE}"
  COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${ESP_HOSTED_SDIO_PATCH}")

find_package(Git REQUIRED QUIET)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn
          "${ESP_HOSTED_SDIO_PATCH}"
  WORKING_DIRECTORY "${ESP_HOSTED_PATCHED_ROOT}"
  RESULT_VARIABLE ESP_HOSTED_SDIO_PATCH_RESULT
  ERROR_VARIABLE ESP_HOSTED_SDIO_PATCH_ERROR)
if(NOT ESP_HOSTED_SDIO_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR
          "Failed to apply ESP-Hosted SDIO patch: ${ESP_HOSTED_SDIO_PATCH_ERROR}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/esp_hosted.cmake)

target_compile_definitions(arch PRIVATE
  ESP_HOSTED_NUTTX=1

  # esp_hosted_os_abstraction.h conditionally changes the OS callback table
  # layout with #ifdef H_USE_MEMPOOL.  Define it consistently for every
  # translation unit even though the NuttX port uses the malloc fallback.

  H_USE_MEMPOOL=0)
if(CONFIG_ESP_HOSTED_SDIO)
  target_compile_definitions(arch PRIVATE
                             CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=1)
endif()

target_include_directories(arch PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${ESP_HAL_3RDPARTY_REPO}/components/esp_wifi/include
  ${ESP_HAL_3RDPARTY_REPO}/components/esp_wifi/include/local
  ${ESP_HOSTED_ROOT}/host
  ${ESP_HOSTED_ROOT}/host/api/include
  ${ESP_HOSTED_ROOT}/host/api/priv
  ${ESP_HOSTED_ROOT}/host/drivers/transport
  ${ESP_HOSTED_ROOT}/host/drivers/transport/spi
  ${ESP_HOSTED_ROOT}/host/drivers/transport/sdio
  ${ESP_HOSTED_ROOT}/host/drivers/transport/spi_hd
  ${ESP_HOSTED_ROOT}/host/drivers/transport/uart
  ${ESP_HOSTED_ROOT}/host/drivers/serial
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/core
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/slaveif
  ${ESP_HOSTED_ROOT}/host/drivers/rpc/wrap
  ${ESP_HOSTED_ROOT}/host/drivers/bt
  ${ESP_HOSTED_ROOT}/host/drivers/power_save
  ${ESP_HOSTED_ROOT}/host/utils
  ${ESP_HOSTED_ROOT}/host/drivers/virtual_serial_if
  ${ESP_HOSTED_ROOT}/common
  ${ESP_HOSTED_ROOT}/common/log
  ${ESP_HOSTED_ROOT}/common/rpc
  ${ESP_HOSTED_ROOT}/common/transport
  ${ESP_HOSTED_PROTOBUF_ROOT}
  ${ESP_HOSTED_ROOT}/common/proto
  ${ESP_HOSTED_ROOT}/common/mempool/include
  ${ESP_HOSTED_ROOT}/common/utils)
