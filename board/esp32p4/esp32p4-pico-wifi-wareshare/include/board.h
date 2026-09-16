/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-pico-wifi-wareshare/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_PICO_WIFI_WARESHARE_INCLUDE_BOARD_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_PICO_WIFI_WARESHARE_INCLUDE_BOARD_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pins used by the GPIO Subsystem */

#define BOARD_NGPIOOUT    2 /* Amount of GPIO Output pins */
#define BOARD_NGPIOINT    1 /* Amount of GPIO Input w/ Interruption pins */

/* ESP32P4-Generic GPIOs ****************************************************/

/* BOOT Button */

#define BUTTON_BOOT  35

/* FT6336 capacitive touch controller (FT5x06 compatible) */

#define BOARD_TOUCH_I2C_PORT       0
#define BOARD_TOUCH_I2C_ADDRESS    0x38
#define BOARD_TOUCH_I2C_FREQUENCY  400000
#define BOARD_TOUCH_RESET_PIN      29
#define BOARD_TOUCH_INTERRUPT_PIN  50

/* ST7796 320x480 SPI LCD */

#define BOARD_LCD_SPI_PORT       2
#define BOARD_LCD_SPI_FREQUENCY  80000000
#define BOARD_LCD_WIDTH          320
#define BOARD_LCD_HEIGHT         480
#define BOARD_LCD_MOSI_PIN       20
#define BOARD_LCD_SCLK_PIN       21
#define BOARD_LCD_MISO_PIN       22
#define BOARD_LCD_CS_PIN         23
#define BOARD_LCD_DC_PIN         26
#define BOARD_LCD_RESET_PIN      27
#define BOARD_LCD_BACKLIGHT_PIN  28

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_PICO_WIFI_WARESHARE_INCLUDE_BOARD_H */
