/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_stickc_display.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_openclaw_node_example_json.h"

static const char *TAG = "stickc_display";
static const char *DEFAULT_HEADING = "OpenClaw";
static const char *DEFAULT_TEXT = "Waiting for display.show from the OpenClaw gateway.";

/* LVGL mutex: protects all lv_* calls from concurrent access between the
 * LVGL task and the OpenClaw command handler. */
static SemaphoreHandle_t s_lvgl_mux = NULL;

/* ---------------------------------------------------------------------- */
/*  SPI bus and ST7789 LCD panel initialisation for the M5StickC Plus2     */
/* ---------------------------------------------------------------------- */

/*
 * The M5StickC Plus2 uses an ST7789v2 1.14" TFT (135x240) over SPI.  Key
 * differences from the ESP-BOX-3 BSP path:
 *
 *   1. No BSP layer; we configure SPI and esp_lcd directly.
 *   2. The 135x240 visible window sits inside the ST7789's native 240x320
 *      framebuffer, requiring gap offsets x=52, y=40 in portrait mode.
 *   3. Backlight on GPIO27 must be driven HIGH explicitly.
 *   4. GPIO4 (power latch) must be held HIGH for battery-powered operation.
 */

static esp_err_t init_power_latch(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << STICKC_PIN_POWER_LATCH,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "power-latch GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(STICKC_PIN_POWER_LATCH, 1), TAG, "power-latch set HIGH failed");
    ESP_LOGI(TAG, "power latch GPIO%d held HIGH", STICKC_PIN_POWER_LATCH);
    return ESP_OK;
}

static esp_err_t init_lcd_backlight(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << STICKC_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "backlight GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(STICKC_LCD_PIN_BL, 1), TAG, "backlight set HIGH failed");
    ESP_LOGI(TAG, "LCD backlight GPIO%d turned ON", STICKC_LCD_PIN_BL);
    return ESP_OK;
}

static esp_err_t create_lcd_panel(esp_lcd_panel_handle_t *out_panel,
                                  esp_lcd_panel_io_handle_t *out_io)
{
    /* SPI bus configuration for the M5StickC Plus2 LCD. */
    spi_bus_config_t buscfg = {
        .sclk_io_num = STICKC_LCD_PIN_SCLK,
        .mosi_io_num = STICKC_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STICKC_LCD_H_RES * 40 * 2,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(STICKC_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
        TAG, "SPI bus init failed");

    /* LCD panel IO on SPI. */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = STICKC_LCD_PIN_DC,
        .cs_gpio_num = STICKC_LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)STICKC_LCD_SPI_HOST,
                                 &io_config, &io_handle),
        TAG, "LCD panel IO create failed");

    /* ST7789 panel configuration.
     *
     * The critical settings for the M5StickC Plus2:
     *   - x_gap = 52, y_gap = 40: the 135x240 visible window is offset
     *     inside the ST7789's 240x320 native resolution.
     *   - rst_gpio: explicit hardware reset before init.
     */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICKC_LCD_PIN_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel),
        TAG, "ST7789 panel create failed");

    /* Reset and initialise the panel. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD panel init failed");

    /* Mirror horizontally and vertically so the display orientation matches
     * the M5StickC Plus2 physical layout (USB-C at the bottom). */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, true, true), TAG, "LCD panel mirror failed");

    /* Swap X/Y to match portrait orientation. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, true), TAG, "LCD panel swap_xy failed");

    /* Apply the gap offsets that centre the 135x240 visible window inside
     * the ST7789's 240x320 native resolution. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, STICKC_LCD_X_GAP, STICKC_LCD_Y_GAP),
                        TAG, "LCD panel set_gap failed");

    /* Turn the display on. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "LCD panel display on failed");

    *out_panel = panel;
    *out_io = io_handle;
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/*  LVGL integration                                                      */
/* ---------------------------------------------------------------------- */

/* LVGL port flush callback: copies pixels from the LVGL buffer to the LCD. */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    int offset_x1 = area->x1;
    int offset_y1 = area->y1;
    int offset_x2 = area->x2;
    int offset_y2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel, offset_x1, offset_y1, offset_x2 + 1, offset_y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

/* LVGL tick timer callback.  Called every 5 ms to keep lv_tick in sync. */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

/* FreeRTOS task that drives the LVGL event loop. */
static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    const TickType_t task_delay = pdMS_TO_TICKS(5);
    for (;;) {
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
        vTaskDelay(task_delay);
    }
}

/* Helper: lock the LVGL mutex with a timeout in milliseconds. */
static bool stickc_lvgl_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void stickc_lvgl_unlock(void)
{
    xSemaphoreGive(s_lvgl_mux);
}

/* ---------------------------------------------------------------------- */
/*  Public API                                                             */
/* ---------------------------------------------------------------------- */

esp_err_t esp_openclaw_node_stickc_display_build_status_payload(
    const esp_openclaw_node_stickc_display_t *display,
    char **out_payload_json)
{
    if (display == NULL || out_payload_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(payload, "ready", display->ready);
    cJSON_AddStringToObject(payload, "heading", display->heading);
    cJSON_AddStringToObject(payload, "text", display->text);
    cJSON_AddNumberToObject(payload, "renderCount", display->render_count);
    cJSON_AddNumberToObject(payload, "lastRenderMs", (double)display->last_render_ms);
    cJSON_AddNumberToObject(payload, "headingMaxLength", STICKC_DISPLAY_MAX_HEADING_LEN);
    cJSON_AddNumberToObject(payload, "textMaxLength", STICKC_DISPLAY_MAX_TEXT_LEN);
    cJSON_AddNumberToObject(payload, "width", STICKC_LCD_H_RES);
    cJSON_AddNumberToObject(payload, "height", STICKC_LCD_V_RES);

    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static void apply_render_locked(esp_openclaw_node_stickc_display_t *display)
{
    lv_label_set_text(display->heading_label, display->heading);
    lv_label_set_text(display->text_label, display->text);
}

esp_err_t esp_openclaw_node_stickc_display_render(
    esp_openclaw_node_stickc_display_t *display,
    const char *heading,
    const char *text)
{
    if (display == NULL || heading == NULL || text == NULL || !display->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!stickc_lvgl_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    snprintf(display->heading, sizeof(display->heading), "%s", heading);
    snprintf(display->text, sizeof(display->text), "%s", text);
    apply_render_locked(display);
    stickc_lvgl_unlock();

    display->render_count += 1U;
    display->last_render_ms = esp_timer_get_time() / 1000LL;
    return ESP_OK;
}

static void create_display_ui_locked(esp_openclaw_node_stickc_display_t *display)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    /* Dark navy background matching the ESP-BOX-3 example. */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    display->container = lv_obj_create(screen);
    lv_obj_set_size(display->container, lv_pct(100), lv_pct(100));
    lv_obj_center(display->container);
    lv_obj_set_style_radius(display->container, 0, 0);
    lv_obj_set_style_border_width(display->container, 0, 0);
    lv_obj_set_style_bg_opa(display->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(display->container, 8, 0);
    lv_obj_set_style_pad_right(display->container, 8, 0);
    lv_obj_set_style_pad_top(display->container, 10, 0);
    lv_obj_set_style_pad_bottom(display->container, 10, 0);
    lv_obj_set_style_pad_row(display->container, 6, 0);
    lv_obj_set_scrollbar_mode(display->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(display->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        display->container,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);

    display->heading_label = lv_label_create(display->container);
    lv_obj_set_width(display->heading_label, lv_pct(100));
    lv_label_set_long_mode(display->heading_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(display->heading_label, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(display->heading_label, &lv_font_montserrat_14, 0);

    display->text_label = lv_label_create(display->container);
    lv_obj_set_width(display->text_label, lv_pct(100));
    lv_label_set_long_mode(display->text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(display->text_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_text_line_space(display->text_label, 4, 0);
    lv_obj_set_style_text_font(display->text_label, &lv_font_montserrat_14, 0);
}

esp_err_t esp_openclaw_node_stickc_display_start(esp_openclaw_node_stickc_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));

    /* Create the LVGL mutex before any peripheral init. */
    s_lvgl_mux = xSemaphoreCreateMutex();
    if (s_lvgl_mux == NULL) {
        ESP_LOGE(TAG, "failed to create LVGL mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Hold the power latch HIGH so the device stays on when powered by battery. */
    ESP_RETURN_ON_ERROR(init_power_latch(), TAG, "power latch init failed");

    /* Create the ST7789 LCD panel with M5StickC Plus2 offsets. */
    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    ESP_RETURN_ON_ERROR(create_lcd_panel(&lcd_panel, &lcd_io), TAG, "LCD panel creation failed");

    /* Turn the backlight on *before* starting LVGL so the user sees
     * something as early as possible.  This is a common blank-screen
     * cause on the M5StickC Plus2: if GPIO27 is never driven HIGH the
     * panel stays dark even though it is rendering correctly. */
    ESP_RETURN_ON_ERROR(init_lcd_backlight(), TAG, "backlight init failed");

    /* Initialize LVGL with the esp_lcd ST7789 panel as the display backend. */
    const int draw_buf_height = CONFIG_STICKC_LCD_DRAW_BUF_HEIGHT;
    lv_display_t *lv_disp = lv_display_create(STICKC_LCD_H_RES, STICKC_LCD_V_RES);
    if (lv_disp == NULL) {
        return ESP_FAIL;
    }

    /* Allocate the draw buffer.  16 bits per pixel (RGB565).
     * Use PSRAM since the M5StickC Plus2 has 2 MB available. */
    size_t draw_buf_size = STICKC_LCD_H_RES * draw_buf_height * 2;
    uint8_t *draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (draw_buf == NULL) {
        /* Fall back to internal DMA-capable memory if PSRAM is unavailable. */
        draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (draw_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate LVGL draw buffer (%zu bytes)", draw_buf_size);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(lv_disp, draw_buf, NULL, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lv_disp, lvgl_flush_cb);
    lv_display_set_user_data(lv_disp, lcd_panel);

    /* Start an esp_timer that increments the LVGL tick every 5 ms. */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "LVGL tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 5000), TAG, "LVGL tick timer start failed"); /* 5000 us = 5 ms */

    display->lv_display = lv_disp;
    display->ready = true;

    /* Render the initial UI under the LVGL lock. */
    if (!stickc_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock timeout during initial UI render");
        return ESP_ERR_TIMEOUT;
    }
    create_display_ui_locked(display);
    stickc_lvgl_unlock();

    /* Start the LVGL task.  It calls lv_timer_handler() in a loop. */
    BaseType_t task_created = xTaskCreate(
        lvgl_task,
        "lvgl",
        4096,
        NULL,
        2,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create LVGL task");
        return ESP_FAIL;
    }

    display->last_render_ms = 0;
    return esp_openclaw_node_stickc_display_render(display, DEFAULT_HEADING, DEFAULT_TEXT);
}
