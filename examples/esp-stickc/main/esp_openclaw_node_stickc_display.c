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
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_node_stickc_hw_node_cmd.h"

static const char *TAG = "stickc_display";
static const char *DEFAULT_HEADING = "OpenClaw";
static const char *DEFAULT_TEXT = "Waiting for display.show from the OpenClaw gateway.";

/* How often the status footer is repainted from live Wi-Fi/gateway state. */
#define STICKC_STATUS_REFRESH_MS 1000

/* Blank the LCD backlight after this long with no new content, to save
 * battery when the device is carried around.  Any display.show from the
 * gateway turns it back on.  The backlight LED is the dominant power draw. */
#define STICKC_BACKLIGHT_IDLE_TIMEOUT_MS 30000

/* How often display.menu samples the buttons while waiting for input. */
#define STICKC_DISPLAY_MENU_POLL_MS 40

/* Buddy idle-animation frame interval. */
#define STICKC_BUDDY_FRAME_MS 220

/* How long the heading/text stay on screen after a button A press or a new
 * display.show; after this they auto-hide so the buddy is alone again. */
#define STICKC_MESSAGE_SHOW_MS 5000

/* LVGL mutex: protects all lv_* calls from concurrent access between the
 * LVGL task and the OpenClaw command handler. */
static SemaphoreHandle_t s_lvgl_mux = NULL;

/* Binary semaphore: given by the SPI DMA done ISR, taken by the LVGL flush
 * callback so it can wait for the transfer to finish before reporting the
 * draw buffer as free.  A counting/binary semaphore (rather than LVGL's own
 * `flushing` flag) avoids the lost-wakeup hang if the ISR fires before LVGL
 * has finished bookkeeping for the flush. */
static SemaphoreHandle_t s_flush_done = NULL;

/* Upper bound on how long the flush callback waits for one SPI transfer.
 * A full 135x20 chunk takes ~1 ms; the timeout is only a safety net so a
 * missed completion degrades to a slow redraw instead of a watchdog reset. */
#define STICKC_FLUSH_TIMEOUT_MS 100

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

/* Drive the LCD backlight and remember its state.  Touches only a GPIO, so
 * it is safe to call from any task. */
static void stickc_set_backlight(esp_openclaw_node_stickc_display_t *display, bool on)
{
    gpio_set_level(STICKC_LCD_PIN_BL, on ? 1 : 0);
    display->backlight_on = on;
}

/* esp_lcd panel-IO callback: fired from the SPI DMA ISR when a colour
 * transfer finishes.  It wakes the flush callback waiting in lvgl_flush_cb(). */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
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
        .on_color_trans_done = notify_lvgl_flush_ready,
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

    /* The ST7789v2 on the M5StickC Plus2 expects display inversion ON;
     * without it every colour renders as a photo-negative. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "LCD panel invert failed");

    /* Portrait 135x240 in the panel's default MADCTL orientation: no axis
     * swap, no mirror.  The LVGL display is created 135 wide x 240 tall to
     * match, and the gap offsets below assume this orientation.  (If the
     * image appears rotated 180 deg, change both flags to true.) */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, false), TAG, "LCD panel swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "LCD panel mirror failed");

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

/* LVGL port flush callback: copies pixels from the LVGL buffer to the LCD.
 *
 * esp_lcd_panel_draw_bitmap() only *queues* an SPI DMA transfer; the pixels
 * are still being read out of px_map after it returns.  We block here on
 * s_flush_done (given by the DMA done ISR) so the buffer is genuinely free
 * before lv_display_flush_ready() is called.  Blocking - rather than letting
 * LVGL spin on its own `flushing` flag - keeps the LVGL task off the CPU
 * while the transfer runs and cannot lose the completion wakeup. */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

    /* LVGL renders RGB565 little-endian, but the ST7789 latches the high
     * byte first.  Without this swap the colours come out scrambled
     * (e.g. the dark-navy background renders as yellow). */
    uint32_t px_count = (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, px_count);

    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(STICKC_FLUSH_TIMEOUT_MS));
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
    /* Wait briefly for the LVGL task to release the lock: a redraw plus its
     * SPI flush is a few ms, so a short bounded wait keeps `display.show`
     * reliable instead of failing the moment a refresh is in progress. */
    if (!stickc_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    snprintf(display->heading, sizeof(display->heading), "%s", heading);
    snprintf(display->text, sizeof(display->text), "%s", text);
    apply_render_locked(display);
    display->render_count += 1U;
    display->last_render_ms = esp_timer_get_time() / 1000LL;
    /* Gateway messages reveal the heading/text line briefly, then auto-hide. */
    display->message_visible_until_ms = display->last_render_ms + STICKC_MESSAGE_SHOW_MS;
    stickc_lvgl_unlock();

    /* New content from the gateway wakes the screen. */
    stickc_set_backlight(display, true);
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/*  On-screen buddy character                                              */
/* ---------------------------------------------------------------------- */

/* Buddy colours. */
#define STICKC_BUDDY_BODY_COLOR  0xfacc15 /* duck yellow */
#define STICKC_BUDDY_BEAK_COLOR  0xf97316 /* orange beak */
#define STICKC_BUDDY_EYE_COLOR   0x0f172a /* dark navy, blends with bg */

/* Blink by hiding the eye briefly.  Must hold the LVGL lock. */
static void buddy_animate_locked(esp_openclaw_node_stickc_display_t *display)
{
    if (display == NULL || display->buddy_eye == NULL) {
        return;
    }
    /* Blink for one frame roughly every 4 seconds. */
    bool blink = (display->buddy_tick % 18U) == 17U;
    if (blink) {
        lv_obj_add_flag(display->buddy_eye, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(display->buddy_eye, LV_OBJ_FLAG_HIDDEN);
    }
}

/* LVGL timer callback: advances the buddy animation and reveals/hides the
 * heading + text labels based on button presses and recent display.show. */
static void buddy_timer_cb(lv_timer_t *timer)
{
    esp_openclaw_node_stickc_display_t *display = lv_timer_get_user_data(timer);
    if (display == NULL) {
        return;
    }
    display->buddy_tick += 1U;
    buddy_animate_locked(display);

    /* Button A reveals the heading/text line and wakes the screen. */
    uint32_t a_now = esp_openclaw_node_stickc_button_press_count(0);
    if (a_now != display->last_button_a_seen) {
        display->last_button_a_seen = a_now;
        const int64_t now_ms = esp_timer_get_time() / 1000LL;
        display->message_visible_until_ms = now_ms + STICKC_MESSAGE_SHOW_MS;
        display->last_render_ms = now_ms;
        stickc_set_backlight(display, true);
    }

    /* Toggle the heading/text labels based on the visibility deadline. */
    bool visible = (esp_timer_get_time() / 1000LL) <= display->message_visible_until_ms;
    if (visible) {
        lv_obj_clear_flag(display->heading_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(display->text_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(display->heading_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(display->text_label, LV_OBJ_FLAG_HIDDEN);
    }
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

    /* Drawn buddy character: yellow body + head + orange beak + dark eye.
     * Positions are absolute inside the buddy container so the duck reads
     * as one coherent shape rather than four widgets in a flex row. */
    display->buddy = lv_obj_create(display->container);
    lv_obj_set_size(display->buddy, lv_pct(100), 90);
    lv_obj_set_style_bg_opa(display->buddy, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(display->buddy, 0, 0);
    lv_obj_set_style_pad_all(display->buddy, 0, 0);
    lv_obj_clear_flag(display->buddy, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *buddy_body = lv_obj_create(display->buddy);
    lv_obj_set_size(buddy_body, 90, 38);
    lv_obj_set_pos(buddy_body, 14, 35);
    lv_obj_set_style_bg_color(buddy_body, lv_color_hex(STICKC_BUDDY_BODY_COLOR), 0);
    lv_obj_set_style_bg_opa(buddy_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(buddy_body, 19, 0); /* capsule */
    lv_obj_set_style_border_width(buddy_body, 0, 0);
    lv_obj_set_style_pad_all(buddy_body, 0, 0);
    lv_obj_clear_flag(buddy_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *buddy_head = lv_obj_create(display->buddy);
    lv_obj_set_size(buddy_head, 35, 35);
    lv_obj_set_pos(buddy_head, 64, 12);
    lv_obj_set_style_bg_color(buddy_head, lv_color_hex(STICKC_BUDDY_BODY_COLOR), 0);
    lv_obj_set_style_bg_opa(buddy_head, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(buddy_head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(buddy_head, 0, 0);
    lv_obj_set_style_pad_all(buddy_head, 0, 0);
    lv_obj_clear_flag(buddy_head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *buddy_beak = lv_obj_create(display->buddy);
    lv_obj_set_size(buddy_beak, 18, 8);
    lv_obj_set_pos(buddy_beak, 98, 26);
    lv_obj_set_style_bg_color(buddy_beak, lv_color_hex(STICKC_BUDDY_BEAK_COLOR), 0);
    lv_obj_set_style_bg_opa(buddy_beak, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(buddy_beak, 3, 0);
    lv_obj_set_style_border_width(buddy_beak, 0, 0);
    lv_obj_set_style_pad_all(buddy_beak, 0, 0);
    lv_obj_clear_flag(buddy_beak, LV_OBJ_FLAG_SCROLLABLE);

    display->buddy_eye = lv_obj_create(display->buddy);
    lv_obj_set_size(display->buddy_eye, 6, 6);
    lv_obj_set_pos(display->buddy_eye, 80, 22);
    lv_obj_set_style_bg_color(display->buddy_eye, lv_color_hex(STICKC_BUDDY_EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(display->buddy_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(display->buddy_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(display->buddy_eye, 0, 0);
    lv_obj_set_style_pad_all(display->buddy_eye, 0, 0);
    lv_obj_clear_flag(display->buddy_eye, LV_OBJ_FLAG_SCROLLABLE);

    display->heading_label = lv_label_create(display->container);
    lv_obj_set_width(display->heading_label, lv_pct(100));
    lv_label_set_long_mode(display->heading_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(display->heading_label, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(display->heading_label, &lv_font_montserrat_14, 0);

    display->text_label = lv_label_create(display->container);
    lv_obj_set_width(display->text_label, lv_pct(100));
    /* Let the body text take all the space between the heading and the
     * footer, so the status footer stays pinned to the bottom edge. */
    lv_obj_set_flex_grow(display->text_label, 1);
    lv_label_set_long_mode(display->text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(display->text_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_text_line_space(display->text_label, 4, 0);
    lv_obj_set_style_text_font(display->text_label, &lv_font_montserrat_14, 0);

    /* Hide the heading/text by default - the buddy owns the main screen.
     * They appear for a few seconds when button A is pressed or when a
     * display.show arrives from the gateway. */
    lv_obj_add_flag(display->heading_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(display->text_label, LV_OBJ_FLAG_HIDDEN);

    /* Connection-warning footer.  Hidden while Wi-Fi and the gateway are both
     * up (the expected state); shown as a single amber line otherwise. */
    display->status_label = lv_label_create(display->container);
    lv_obj_set_width(display->status_label, lv_pct(100));
    lv_label_set_long_mode(display->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(display->status_label, lv_color_hex(0xfbbf24), 0);
    lv_obj_set_style_text_font(display->status_label, &lv_font_montserrat_14, 0);
    /* A thin top rule separates the warning from the body text. */
    lv_obj_set_style_border_color(display->status_label, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_side(display->status_label, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(display->status_label, 1, 0);
    lv_obj_set_style_pad_top(display->status_label, 5, 0);
}

/* Update the connection-warning footer.  Must be called with the LVGL lock
 * held.  The footer is hidden when Wi-Fi and the gateway are both up - the
 * expected state - so the whole screen is free for content; otherwise it
 * shows a single line naming whichever link is down.  Wi-Fi down implies the
 * gateway is unreachable, so only the Wi-Fi warning is shown in that case. */
static void refresh_status_footer_locked(esp_openclaw_node_stickc_display_t *display)
{
    if (display == NULL || display->status_label == NULL) {
        return;
    }

    const char *warning = NULL;
    if (!esp_openclaw_node_wifi_is_connected()) {
        warning = "Wi-Fi not connected";
    } else if (!display->gateway_connected) {
        warning = "Gateway not connected";
    }

    if (warning == NULL) {
        lv_obj_add_flag(display->status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(display->status_label, warning);
        lv_obj_clear_flag(display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* LVGL timer callback: runs inside lv_timer_handler(), already under the
 * LVGL lock, so it can repaint the footer directly.  Also blanks the
 * backlight once the screen has been idle long enough. */
static void status_refresh_timer_cb(lv_timer_t *timer)
{
    esp_openclaw_node_stickc_display_t *display = lv_timer_get_user_data(timer);
    refresh_status_footer_locked(display);

    if (display != NULL && display->backlight_on) {
        int64_t idle_ms = esp_timer_get_time() / 1000LL - display->last_render_ms;
        if (idle_ms > STICKC_BACKLIGHT_IDLE_TIMEOUT_MS) {
            stickc_set_backlight(display, false);
        }
    }
}

esp_err_t esp_openclaw_node_stickc_display_start(esp_openclaw_node_stickc_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));

    /* Create the LVGL mutex and the SPI flush semaphore before any peripheral
     * init, so the panel IO callback and flush callback can rely on them. */
    s_lvgl_mux = xSemaphoreCreateMutex();
    s_flush_done = xSemaphoreCreateBinary();
    if (s_lvgl_mux == NULL || s_flush_done == NULL) {
        ESP_LOGE(TAG, "failed to create LVGL synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    /* Hold the power latch HIGH so the device stays on when powered by battery. */
    ESP_RETURN_ON_ERROR(init_power_latch(), TAG, "power latch init failed");

    /* Initialise the LVGL core.  This must run before any other lv_* call. */
    lv_init();

    /* Create the LVGL display object. */
    lv_display_t *lv_disp = lv_display_create(STICKC_LCD_H_RES, STICKC_LCD_V_RES);
    if (lv_disp == NULL) {
        ESP_LOGE(TAG, "failed to create LVGL display");
        return ESP_FAIL;
    }

    /* Create the ST7789 LCD panel with M5StickC Plus2 offsets. */
    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    ESP_RETURN_ON_ERROR(create_lcd_panel(&lcd_panel, &lcd_io), TAG, "LCD panel creation failed");

    /* Turn the backlight on *before* starting LVGL so the user sees
     * something as early as possible.  This is a common blank-screen
     * cause on the M5StickC Plus2: if GPIO27 is never driven HIGH the
     * panel stays dark even though it is rendering correctly. */
    ESP_RETURN_ON_ERROR(init_lcd_backlight(), TAG, "backlight init failed");
    display->backlight_on = true;

    /* Allocate the LVGL draw buffer: CONFIG_STICKC_LCD_DRAW_BUF_HEIGHT rows
     * at 16 bits per pixel (RGB565).  The buffer is handed straight to the
     * SPI DMA engine, so it must live in internal DMA-capable RAM.  At
     * 135 x 20 x 2 bytes this is only ~5 KB, so PSRAM is not needed. */
    const int draw_buf_height = CONFIG_STICKC_LCD_DRAW_BUF_HEIGHT;
    size_t draw_buf_size = STICKC_LCD_H_RES * draw_buf_height * 2;
    uint8_t *draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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

    /* Build the UI, seed the boot screen, and start the footer refresh timer,
     * all under the LVGL lock *before* the LVGL task exists.  Seeding the
     * content here - rather than via a post-startup render() call - avoids a
     * race against the task's first refresh. */
    if (!stickc_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock timeout during initial UI render");
        return ESP_ERR_TIMEOUT;
    }
    create_display_ui_locked(display);
    snprintf(display->heading, sizeof(display->heading), "%s", DEFAULT_HEADING);
    snprintf(display->text, sizeof(display->text), "%s", DEFAULT_TEXT);
    apply_render_locked(display);
    refresh_status_footer_locked(display);
    lv_timer_create(status_refresh_timer_cb, STICKC_STATUS_REFRESH_MS, display);
    lv_timer_create(buddy_timer_cb, STICKC_BUDDY_FRAME_MS, display);
    stickc_lvgl_unlock();

    display->render_count = 1U;
    display->last_render_ms = esp_timer_get_time() / 1000LL;

    /* Start the LVGL task.  It calls lv_timer_handler() in a loop.
     * 8 KB of stack: LVGL 9 glyph rendering and flex layout overflow 4 KB. */
    BaseType_t task_created = xTaskCreate(
        lvgl_task,
        "lvgl",
        8192,
        NULL,
        2,
        NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create LVGL task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void esp_openclaw_node_stickc_display_set_gateway_connected(
    esp_openclaw_node_stickc_display_t *display,
    bool connected)
{
    if (display == NULL) {
        return;
    }
    /* A plain aligned store; the footer refresh timer picks it up on its
     * next tick, so no LVGL lock is needed here. */
    display->gateway_connected = connected;
}

/* Render the menu into the heading and body labels.  Must hold the LVGL lock.
 * The selected option is marked with "> "; long options are truncated. */
static void render_menu_locked(esp_openclaw_node_stickc_display_t *display,
                               const char *title,
                               const char *const *options,
                               int option_count,
                               int selected)
{
    lv_label_set_text(display->heading_label,
                      (title != NULL && title[0] != '\0') ? title : "Menu");

    char body[512];
    size_t pos = 0;
    for (int i = 0; i < option_count; ++i) {
        int written = snprintf(body + pos, sizeof(body) - pos, "%s%.24s%s",
                               (i == selected) ? "> " : "  ",
                               options[i] != NULL ? options[i] : "",
                               (i < option_count - 1) ? "\n" : "");
        if (written < 0 || (size_t)written >= sizeof(body) - pos) {
            break;
        }
        pos += (size_t)written;
    }
    lv_label_set_text(display->text_label, body);
}

esp_err_t esp_openclaw_node_stickc_display_run_menu(
    esp_openclaw_node_stickc_display_t *display,
    const char *title,
    const char *const *options,
    int option_count,
    uint32_t timeout_ms,
    int *out_selected)
{
    if (display == NULL || options == NULL || out_selected == NULL ||
        option_count < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_selected = -1;

    int selected = 0;
    uint32_t a_seen = esp_openclaw_node_stickc_button_press_count(0);
    uint32_t b_seen = esp_openclaw_node_stickc_button_press_count(1);

    if (!stickc_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    /* The menu reuses heading/text labels, so make sure they're visible
     * for the whole menu duration. */
    lv_obj_clear_flag(display->heading_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(display->text_label, LV_OBJ_FLAG_HIDDEN);
    display->message_visible_until_ms =
        (esp_timer_get_time() / 1000LL) + (int64_t)timeout_ms + 2000;
    render_menu_locked(display, title, options, option_count, selected);
    stickc_set_backlight(display, true);
    stickc_lvgl_unlock();

    const int64_t deadline = esp_timer_get_time() / 1000LL + (int64_t)timeout_ms;
    bool chosen = false;
    while (esp_timer_get_time() / 1000LL < deadline) {
        uint32_t a = esp_openclaw_node_stickc_button_press_count(0);
        uint32_t b = esp_openclaw_node_stickc_button_press_count(1);
        if (b != b_seen) {
            chosen = true; /* button B confirms */
            break;
        }
        if (a != a_seen) {
            /* button A advances the highlight, honouring fast multi-presses */
            selected = (int)(((uint32_t)selected + (a - a_seen)) % (uint32_t)option_count);
            a_seen = a;
            if (stickc_lvgl_lock(1000)) {
                render_menu_locked(display, title, options, option_count, selected);
                stickc_lvgl_unlock();
            }
        }
        /* Keep the screen lit while the menu is open. */
        display->last_render_ms = esp_timer_get_time() / 1000LL;
        vTaskDelay(pdMS_TO_TICKS(STICKC_DISPLAY_MENU_POLL_MS));
    }

    /* Restore the pre-menu screen (heading/text were never modified) and
     * snap the labels back to hidden so the buddy is alone again. */
    if (stickc_lvgl_lock(1000)) {
        apply_render_locked(display);
        display->message_visible_until_ms = 0;
        lv_obj_add_flag(display->heading_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(display->text_label, LV_OBJ_FLAG_HIDDEN);
        stickc_lvgl_unlock();
    }

    if (chosen) {
        *out_selected = selected;
    }
    return ESP_OK;
}
