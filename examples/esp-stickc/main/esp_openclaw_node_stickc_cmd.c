/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_stickc_cmd.h"

#include "esp_openclaw_node_stickc_display_node_cmd.h"
#include "esp_openclaw_node_stickc_hw_node_cmd.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "esp_check.h"

static const char *TAG = "stickc_cmd";

esp_err_t esp_openclaw_node_stickc_register_node_commands(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_stickc_display_t *display)
{
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_common_register_device_node_commands(node),
        TAG,
        "registering device commands failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_stickc_register_display_node_commands(node, display),
        TAG,
        "registering display commands failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_stickc_register_hw_node_commands(node),
        TAG,
        "registering hardware commands failed");
    return ESP_OK;
}
