/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_openclaw_node.h"

/**
 * @brief Register the M5StickC Plus2 on-board hardware commands.
 *
 * Adds four capabilities, each backed by a single command:
 *   - `imu`     / `imu.read`       MPU6886 accelerometer, gyroscope, temperature
 *   - `buzzer`  / `buzzer.beep`    play a tone on the passive buzzer
 *   - `led`     / `led.set`        drive the on-board red LED
 *   - `battery` / `battery.status` battery voltage and rough charge percent
 *
 * Peripheral bring-up is best-effort: if a sensor fails to initialize its
 * command is still registered but returns an `UNAVAILABLE` error when invoked.
 *
 * @param[in] node OpenClaw Node instance to extend.
 *
 * @return
 *      - `ESP_OK` on success
 *      - an ESP-IDF error code if capability or command registration fails
 */
esp_err_t esp_openclaw_node_stickc_register_hw_node_commands(esp_openclaw_node_handle_t node);
