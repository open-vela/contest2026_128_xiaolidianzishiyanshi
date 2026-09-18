# ##############################################################################
# arch/risc-v/src/esp32p4/CMakeLists.txt
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

add_subdirectory(espressif espressif)

set(SRCS esp_chip_rev.c esp_clic.c esp_fpu.c)

if(CONFIG_ARCH_FPU)
  target_link_options(nuttx PRIVATE -Wl,--wrap=riscv_restorefpu)
endif()

if(CONFIG_ESPRESSIF_MIPI_DSI)
  list(APPEND SRCS ${CMAKE_CURRENT_SOURCE_DIR}/esp_mipi_dsi.c)
endif()

if (CONFIG_ESP32P4_SDMMC)
  list(APPEND SRCS ${CMAKE_CURRENT_SOURCE_DIR}/esp32p4_sdmmc.c)
endif()

if(CONFIG_ESP32P4_PPA)
  list(APPEND SRCS ${CMAKE_CURRENT_SOURCE_DIR}/esp32p4_ppa.c
       ${CMAKE_CURRENT_SOURCE_DIR}/esp32p4_ppa_hal.c
       ${CMAKE_CURRENT_SOURCE_DIR}/esp32p4_ppa_dma2d.c)
endif()

target_compile_definitions(arch PRIVATE _RETARGETABLE_LOCKING)
target_compile_options(arch PRIVATE -Wno-array-bounds -Wno-stringop-overflow)

if(SRCS)
  target_sources(arch PRIVATE ${SRCS})
endif()
