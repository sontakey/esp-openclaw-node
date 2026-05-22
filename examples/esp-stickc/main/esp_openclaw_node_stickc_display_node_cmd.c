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

/* display.menu timeout bounds, in milliseconds. */
#define STICKC_MENU_DEFAULT_TIMEOUT_MS 30000
#define STICKC_MENU_MIN_TIMEOUT_MS     1000
#define STICKC_MENU_MAX_TIMEOUT_MS     120000

static esp_err_t handle_display_menu(
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
    cJSON *params = NULL;
    esp_err_t err = esp_openclaw_node_example_parse_json_params(params_json, &params, out_error);
    if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "invalid params");
    }

    cJSON *options_json = cJSON_GetObjectItemCaseSensitive(params, "options");
    if (!cJSON_IsArray(options_json)) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "params must include an 'options' string array";
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    int option_count = cJSON_GetArraySize(options_json);
    if (option_count < 1 || option_count > STICKC_DISPLAY_MENU_MAX_OPTIONS) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "options must hold 1 to 8 string entries";
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }

    const char *options[STICKC_DISPLAY_MENU_MAX_OPTIONS];
    for (int i = 0; i < option_count; ++i) {
        cJSON *item = cJSON_GetArrayItem(options_json, i);
        if (!cJSON_IsString(item) || item->valuestring == NULL) {
            out_error->code = "INVALID_PARAMS";
            out_error->message = "every option must be a string";
            cJSON_Delete(params);
            return ESP_ERR_INVALID_ARG;
        }
        options[i] = item->valuestring;
    }

    const char *title = "";
    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(params, "title");
    if (cJSON_IsString(title_json) && title_json->valuestring != NULL) {
        title = title_json->valuestring;
    }

    uint32_t timeout_ms = STICKC_MENU_DEFAULT_TIMEOUT_MS;
    cJSON *timeout_json = cJSON_GetObjectItemCaseSensitive(params, "timeoutMs");
    if (cJSON_IsNumber(timeout_json)) {
        double requested = timeout_json->valuedouble;
        if (requested < STICKC_MENU_MIN_TIMEOUT_MS) {
            requested = STICKC_MENU_MIN_TIMEOUT_MS;
        } else if (requested > STICKC_MENU_MAX_TIMEOUT_MS) {
            requested = STICKC_MENU_MAX_TIMEOUT_MS;
        }
        timeout_ms = (uint32_t)requested;
    }

    /* Blocks until the user picks an option (button B) or the timeout fires. */
    int selected = -1;
    err = esp_openclaw_node_stickc_display_run_menu(
        display, title, options, option_count, timeout_ms, &selected);
    if (err != ESP_OK) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "display menu is not available";
        cJSON_Delete(params);
        return err;
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    if (selected >= 0) {
        cJSON_AddBoolToObject(payload, "timedOut", false);
        cJSON_AddNumberToObject(payload, "selected", selected);
        cJSON_AddStringToObject(payload, "label", options[selected]);
    } else {
        cJSON_AddBoolToObject(payload, "timedOut", true);
    }
    cJSON_Delete(params);
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
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
    static const esp_openclaw_node_command_t DISPLAY_MENU_COMMAND = {
        .name = "display.menu",
        .handler = handle_display_menu,
        .context = NULL,
    };

    esp_openclaw_node_command_t show_command = DISPLAY_SHOW_COMMAND;
    show_command.context = display;
    esp_openclaw_node_command_t status_command = DISPLAY_STATUS_COMMAND;
    status_command.context = display;
    esp_openclaw_node_command_t menu_command = DISPLAY_MENU_COMMAND;
    menu_command.context = display;

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
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &menu_command),
        TAG,
        "registering display.menu failed");
    return ESP_OK;
}
