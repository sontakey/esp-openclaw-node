/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_openclaw_node_stickc_display.h"
#include "esp_openclaw_node.h"

/**
 * @brief Register the display-specific OpenClaw commands for the M5StickC Plus2.
 *
 * The helper adds the `display.show` and `display.status` commands.
 *
 * @param[in] node OpenClaw Node instance to extend.
 * @param[in] display Display state used by the command handlers.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `display` is `NULL`
 *      - an ESP-IDF error code if registration fails
 */
esp_err_t esp_openclaw_node_stickc_register_display_node_commands(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_stickc_display_t *display);
