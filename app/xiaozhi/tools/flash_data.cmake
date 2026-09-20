# Flash only the xiaozhi LittleFS data partition.  This is intentionally kept
# separate from the normal firmware flash target so repeated nuttx.bin updates
# preserve /data.

if(NOT DEFINED XIAOZHI_DATA_IMAGE OR
   NOT EXISTS "${XIAOZHI_DATA_IMAGE}")
  message(FATAL_ERROR "LittleFS image not found: ${XIAOZHI_DATA_IMAGE}")
endif()

if("$ENV{ESPTOOL_PORT}" STREQUAL "")
  message(FATAL_ERROR
    "ESPTOOL_PORT is not set. Example: ESPTOOL_PORT=/dev/ttyACM0 cmake --build <builddir> -t xiaozhi_data_flash")
endif()

find_program(ESPTOOL esptool esptool.py REQUIRED)

if("$ENV{ESPTOOL_BAUD}" STREQUAL "")
  set(ESPTOOL_BAUD 921600)
else()
  set(ESPTOOL_BAUD "$ENV{ESPTOOL_BAUD}")
endif()

execute_process(
  COMMAND ${ESPTOOL}
          -c esp32p4 -p "$ENV{ESPTOOL_PORT}" -b ${ESPTOOL_BAUD}
          write-flash -fs 16MB -fm dio -ff 80m
          0xC00000 "${XIAOZHI_DATA_IMAGE}"
  RESULT_VARIABLE FLASH_RESULT)

if(NOT FLASH_RESULT EQUAL 0)
  message(FATAL_ERROR "xiaozhi data partition flash failed: ${FLASH_RESULT}")
endif()

message(STATUS "Flashed xiaozhi LittleFS data partition at 0xC00000")
