# The application image is flashed at 0x2000 and /data starts at 0xC00000.
# Fail the normal build before a future firmware growth can silently overwrite
# the persistent filesystem.

if(NOT DEFINED FIRMWARE_IMAGE OR NOT EXISTS "${FIRMWARE_IMAGE}")
  message(FATAL_ERROR "Firmware image not found: ${FIRMWARE_IMAGE}")
endif()

set(FIRMWARE_OFFSET 0x2000)
set(DATA_OFFSET 0xC00000)
file(SIZE "${FIRMWARE_IMAGE}" FIRMWARE_SIZE)
math(EXPR FIRMWARE_END "${FIRMWARE_OFFSET} + ${FIRMWARE_SIZE}"
     OUTPUT_FORMAT HEXADECIMAL)

if(FIRMWARE_END GREATER DATA_OFFSET)
  message(FATAL_ERROR
    "nuttx.bin ends at ${FIRMWARE_END} and overlaps /data at 0xC00000")
endif()

message(STATUS
  "nuttx.bin ends at ${FIRMWARE_END}; persistent /data starts at 0xC00000")
