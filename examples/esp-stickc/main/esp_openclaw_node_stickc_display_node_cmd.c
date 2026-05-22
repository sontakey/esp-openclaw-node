/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_stickc_display_node_cmd.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_openclaw_node_example_json.h"

static const char *TAG = "stickc_display_cmd";

static esp_err_t parse_required_text(
    cJSON *params,
    const char *name,
    size_t max_len,
    const char **out_value,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsString(field) || field->valuestring == NULL) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "display params must include string heading and text fields";
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(field->valuestring) > max_len) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "display field exceeds maximum supported length";
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = field->valuestring;
    return ESP_OK;
}

static esp_err_t handle_display_show(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)params_len;

    esp_openclaw_node_stickc_display_t *display = (esp_openclaw_node_stickc_display_t *)context;
    const char *heading = NULL;
    const char *text = NULL;
    cJSON *params = NULL;

    esp_err_t err = esp_openclaw_node_example_parse_json_params(params_json, &params, out_error);
    if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "invalid params");
    }

    err = parse_required_text(
        params,
        "heading",
        STICKC_DISPLAY_MAX_HEADING_LEN,
        &heading,
        out_error);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        ESP_RETURN_ON_ERROR(err, TAG, "invalid heading");
    }
    err = parse_required_text(
        params,
        "text",
        STICKC_DISPLAY_MAX_TEXT_LEN,
        &text,
        out_error);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        ESP_RETURN_ON_ERROR(err, TAG, "invalid text");
    }

    if (esp_openclaw_node_stickc_display_render(display, heading, text) != ESP_OK) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "display renderer is not ready";
        cJSON_Delete(params);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON_Delete(params);
    return esp_openclaw_node_stickc_display_build_status_payload(display, out_payload_json);
}

static esp_err_t handle_display_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)params_json;
    (void)params_len;
    (void)out_error;

    esp_openclaw_node_stickc_display_t *display = (esp_openclaw_node_stickc_display_t *)context;
    return esp_openclaw_node_stickc_display_build_status_payload(display, out_payload_json);
}

esp_err_t esp_openclaw_node_stickc_register_display_node_commands(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_stickc_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const esp_openclaw_node_command_t DISPLAY_SHOW_COMMAND = {
        .name = "display.show",
        .handler = handle_display_show,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t DISPLAY_STATUS_COMMAND = {
        .name = "display.status",
        .handler = handle_display_status,
        .context = NULL,
    };

    esp_openclaw_node_command_t show_command = DISPLAY_SHOW_COMMAND;
    show_command.context = display;
    esp_openclaw_node_command_t status_command = DISPLAY_STATUS_COMMAND;
    status_command.context = display;

    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node, "display"),
        TAG,
        "registering display capability failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &show_command),
        TAG,
        "registering display.show failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &status_command),
        TAG,
        "registering display.status failed");
    return ESP_OK;
}
