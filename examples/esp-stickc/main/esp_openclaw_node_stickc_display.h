/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Maximum UTF-8 heading length accepted by `display.show`. */
#define STICKC_DISPLAY_MAX_HEADING_LEN 64
/** @brief Maximum UTF-8 body length accepted by `display.show`. */
#define STICKC_DISPLAY_MAX_TEXT_LEN 512

/*
 * M5StickC Plus2 LCD hardware constants.
 * Display: 1.14" TFT, 135x240, ST7789v2 over SPI.
 *
 * The ST7789 in the M5StickC Plus2 has a 240x320 internal framebuffer, but
 * only a 135x240 region is visible.  The panel requires column/row gap offsets
 * to centre the visible area: x_gap = 52, y_gap = 40 (portrait, default
 * MADCTL rotation).  These offsets are applied in esp_lcd_panel_dev_config_t.
 */
#define STICKC_LCD_H_RES  135
#define STICKC_LCD_V_RES  240
#define STICKC_LCD_X_GAP  52
#define STICKC_LCD_Y_GAP  40

/* SPI pin assignments on the M5StickC Plus2. */
#define STICKC_LCD_SPI_HOST    HSPI_HOST
#define STICKC_LCD_PIN_SCLK    13
#define STICKC_LCD_PIN_MOSI    15
#define STICKC_LCD_PIN_CS      5
#define STICKC_LCD_PIN_DC      14
#define STICKC_LCD_PIN_RST     12
#define STICKC_LCD_PIN_BL      27

/* Power latch: hold HIGH for battery operation on the Plus2. */
#define STICKC_PIN_POWER_LATCH 4

/* Forward declarations matching LVGL 9.x's own typedefs in lv_types.h, so
 * this header can be included with or without lvgl.h already in scope. */
typedef struct lv_display_t lv_display_t;
typedef struct lv_obj_t lv_obj_t;

/**
 * @brief Display state shared by the M5StickC Plus2 example modules.
 */
typedef struct {
    bool ready; /**< Whether the display runtime has been initialized successfully. */
    char heading[STICKC_DISPLAY_MAX_HEADING_LEN + 1]; /**< Last rendered heading text. */
    char text[STICKC_DISPLAY_MAX_TEXT_LEN + 1]; /**< Last rendered body text. */
    uint32_t render_count; /**< Number of successful render operations since boot. */
    int64_t last_render_ms; /**< Timestamp of the most recent render in ms since boot. */
    lv_display_t *lv_display; /**< Underlying LVGL display handle owned by the example. */
    lv_obj_t *container; /**< Root LVGL container for the example screen. */
    lv_obj_t *heading_label; /**< LVGL label used for the heading line. */
    lv_obj_t *text_label; /**< LVGL label used for the body text block. */
} esp_openclaw_node_stickc_display_t;

/**
 * @brief Initialize the M5StickC Plus2 display runtime and render the boot screen.
 *
 * Configures SPI, creates the ST7789 LCD panel with the correct offsets
 * (x_gap=52, y_gap=40), enables backlight on GPIO27, and starts LVGL.
 * Also holds the power-latch GPIO4 HIGH so the device stays on when on battery.
 *
 * @param[out] display Display state to initialize.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if `display` is `NULL`
 *      - an ESP-IDF error code if SPI, LCD panel, or LVGL setup fails
 */
esp_err_t esp_openclaw_node_stickc_display_start(esp_openclaw_node_stickc_display_t *display);

/**
 * @brief Render new heading and body text on the display.
 *
 * @param[in,out] display Initialized display state.
 * @param[in] heading Short heading text.
 * @param[in] text Body text.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if any argument is invalid
 *      - `ESP_ERR_INVALID_STATE` if the display is not ready
 *      - `ESP_ERR_TIMEOUT` if the LVGL mutex cannot be acquired
 */
esp_err_t esp_openclaw_node_stickc_display_render(
    esp_openclaw_node_stickc_display_t *display,
    const char *heading,
    const char *text);

/**
 * @brief Build the JSON payload returned by `display.status` and `display.show`.
 *
 * Ownership of the returned buffer transfers to the caller, which must free it
 * with a `malloc()`-compatible allocator.
 *
 * @param[in] display Display state to serialize.
 * @param[out] out_payload_json Allocated UTF-8 JSON string on success.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if any argument is invalid
 *      - `ESP_ERR_NO_MEM` if allocation fails
 */
esp_err_t esp_openclaw_node_stickc_display_build_status_payload(
    const esp_openclaw_node_stickc_display_t *display,
    char **out_payload_json);
