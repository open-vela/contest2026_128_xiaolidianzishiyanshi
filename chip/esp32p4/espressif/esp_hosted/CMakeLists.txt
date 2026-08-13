# ##############################################################################
# arch/risc-v/src/common/espressif/esp_hosted/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(NOT CONFIG_ESP_HOSTED)
  return()
endif()

# ##############################################################################
# Fetch esp-hosted-mcu source
# ##############################################################################

set(ESP_HOSTED_REPO_NAME esp-hosted-mcu)

if(DEFINED ENV{ESP_HOSTED_URL})
  set(ESP_HOSTED_URL $ENV{ESP_HOSTED_URL})
else()
  set(ESP_HOSTED_URL "https://github.com/espressif/esp-hosted-mcu.git")
endif()

if(DEFINED ENV{ESP_HOSTED_VERSION})
  set(ESP_HOSTED_VERSION $ENV{ESP_HOSTED_VERSION})
else()
  set(ESP_HOSTED_VERSION "main")
endif()

set(ESP_HOSTED_REPO_DIR
    "${CMAKE_BINARY_DIR}/arch/risc-v/src/common/espressif/${ESP_HOSTED_REPO_NAME}")

if(NOT IS_DIRECTORY "${ESP_HOSTED_REPO_DIR}")
  message(STATUS "Cloning esp-hosted-mcu from ${ESP_HOSTED_URL}")
  FetchContent_Declare(
    esp_hosted_mcu
    GIT_REPOSITORY ${ESP_HOSTED_URL}
    GIT_TAG        ${ESP_HOSTED_VERSION}
    SOURCE_DIR     ${ESP_HOSTED_REPO_DIR}
    GIT_SUBMODULES "common/protobuf-c"
  )

  # Use FetchContent_Populate instead of FetchContent_MakeAvailable to avoid
  # processing the upstream CMakeLists.txt, which calls ESP-IDF-specific
  # functions (e.g. idf_component_register) that are not available in the
  # NuttX build environment.  The port's own esp_hosted.cmake handles all
  # compilation for NuttX.
  FetchContent_GetProperties(esp_hosted_mcu)
  if(NOT esp_hosted_mcu_POPULATED)
    FetchContent_Populate(esp_hosted_mcu)
  endif()
endif()

set(ESP_HOSTED_ROOT ${ESP_HOSTED_REPO_DIR})

# ##############################################################################
# Include source file list
# ##############################################################################

include(${CMAKE_CURRENT_LIST_DIR}/esp_hosted.cmake)

# ##############################################################################
# Compile definitions
# ##############################################################################

target_compile_definitions(arch PRIVATE
  ESP_HOSTED_NUTTX=1
)

# ##############################################################################
# Include paths
# ##############################################################################

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
  ${ESP_HOSTED_ROOT}/common/protobuf-c
  ${ESP_HOSTED_ROOT}/common/proto
  ${ESP_HOSTED_ROOT}/common/mempool/include
  ${ESP_HOSTED_ROOT}/common/utils
)
