/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_stickc_hw_node_cmd.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_openclaw_node_example_json.h"

static const char *TAG = "stickc_hw";

/* ---------------------------------------------------------------------- */
/*  M5StickC Plus2 on-board peripheral pin map                            */
/* ---------------------------------------------------------------------- */

/* On-board red LED.  GPIO19 on the M5StickC Plus2 - note GPIO10 (used on the
 * original M5StickC) is an embedded-flash pin on the ESP32-PICO-V3-02 and must
 * not be reconfigured.  Flip STICKC_HW_LED_ACTIVE_LOW if on/off come out
 * inverted on a given board revision. */
#define STICKC_HW_LED_GPIO         GPIO_NUM_19
#define STICKC_HW_LED_ACTIVE_LOW   0

/* Passive buzzer, driven with a PWM square wave. */
#define STICKC_HW_BUZZER_GPIO      GPIO_NUM_2

/* Internal I2C bus shared by the MPU6886 IMU and the RTC. */
#define STICKC_HW_I2C_PORT         I2C_NUM_0
#define STICKC_HW_I2C_SDA_GPIO     GPIO_NUM_21
#define STICKC_HW_I2C_SCL_GPIO     GPIO_NUM_22

/* MPU6886 6-axis IMU. */
#define MPU6886_I2C_ADDR           0x68
#define MPU6886_REG_WHO_AM_I       0x75
#define MPU6886_WHO_AM_I_VALUE     0x19
#define MPU6886_REG_PWR_MGMT_1     0x6B
#define MPU6886_REG_CONFIG         0x1A
#define MPU6886_REG_GYRO_CONFIG    0x1B
#define MPU6886_REG_ACCEL_CONFIG   0x1C
#define MPU6886_REG_ACCEL_XOUT_H   0x3B /* accel[6] + temp[2] + gyro[6] */
#define MPU6886_ACCEL_FS_G         8.0f    /* ACCEL_CONFIG 0x10 -> +/-8 g     */
#define MPU6886_GYRO_FS_DPS        2000.0f /* GYRO_CONFIG  0x18 -> +/-2000 dps */

/* Buzzer LEDC configuration. */
#define STICKC_HW_BUZZER_TIMER     LEDC_TIMER_0
#define STICKC_HW_BUZZER_CHANNEL   LEDC_CHANNEL_0
#define STICKC_HW_BUZZER_DUTY_ON   512 /* 50% of the 10-bit (1024) range */
#define STICKC_HW_BEEP_DEFAULT_HZ  4000
#define STICKC_HW_BEEP_DEFAULT_MS  200
#define STICKC_HW_BEEP_MAX_MS      1000
#define STICKC_HW_BEEP_MIN_HZ      100
#define STICKC_HW_BEEP_MAX_HZ      10000

/* Battery: ADC1 channel 2 (GPIO38), behind a 1:2 divider on the Plus2. */
#define STICKC_HW_BATTERY_CHANNEL  ADC_CHANNEL_2 /* GPIO38 */
#define STICKC_HW_BATTERY_DIVIDER  2.0f
#define STICKC_HW_BATTERY_FULL_MV  4200
#define STICKC_HW_BATTERY_EMPTY_MV 3000
#define STICKC_HW_BATTERY_SAMPLES  16

/* Buttons A and B.  GPIO37/39 are input-only with no internal pulls; the
 * board has external pull-ups, so a pressed button reads LOW. */
#define STICKC_HW_BTN_A_GPIO       GPIO_NUM_37
#define STICKC_HW_BTN_B_GPIO       GPIO_NUM_39
#define STICKC_HW_BTN_COUNT        2
#define STICKC_HW_BTN_POLL_MS      20

/* motion.status derived from the IMU. */
#define STICKC_HW_RAD_TO_DEG       57.295779f
#define STICKC_HW_MOTION_MOVING_DPS 40.0 /* gyro magnitude above this = moving */

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_imu_dev;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_imu_ready;
static bool s_adc_ready;
static bool s_adc_cali_ready;

/* Button debounce state: updated by button_task(), read by handle_button_status().
 * press_count is cumulative since boot so a polling gateway can diff it. */
typedef struct {
    gpio_num_t gpio;
    volatile bool pressed;
    volatile uint32_t press_count;
} stickc_button_t;
static stickc_button_t s_buttons[STICKC_HW_BTN_COUNT] = {
    {.gpio = STICKC_HW_BTN_A_GPIO},
    {.gpio = STICKC_HW_BTN_B_GPIO},
};
static bool s_buttons_ready;

/* ---------------------------------------------------------------------- */
/*  Peripheral bring-up (best-effort)                                     */
/* ---------------------------------------------------------------------- */

static esp_err_t mpu6886_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_imu_dev, buf, sizeof(buf), 100);
}

static esp_err_t mpu6886_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, out, len, 100);
}

static void init_imu(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = STICKC_HW_I2C_PORT,
        .sda_io_num = STICKC_HW_I2C_SDA_GPIO,
        .scl_io_num = STICKC_HW_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_config, &s_i2c_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed; imu.read disabled");
        return;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6886_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_imu_dev) != ESP_OK) {
        ESP_LOGW(TAG, "I2C device add failed; imu.read disabled");
        return;
    }

    uint8_t who = 0;
    if (mpu6886_read(MPU6886_REG_WHO_AM_I, &who, 1) != ESP_OK ||
        who != MPU6886_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "MPU6886 not detected (WHO_AM_I=0x%02x); imu.read disabled", who);
        return;
    }

    /* Reset, wake on the PLL clock, then select the full-scale ranges. */
    mpu6886_write(MPU6886_REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    mpu6886_write(MPU6886_REG_PWR_MGMT_1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    mpu6886_write(MPU6886_REG_ACCEL_CONFIG, 0x10); /* +/-8 g      */
    mpu6886_write(MPU6886_REG_GYRO_CONFIG, 0x18);  /* +/-2000 dps */
    mpu6886_write(MPU6886_REG_CONFIG, 0x01);       /* enable DLPF */
    vTaskDelay(pdMS_TO_TICKS(10));

    s_imu_ready = true;
    ESP_LOGI(TAG, "MPU6886 IMU ready");
}

static void init_buzzer(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = STICKC_HW_BUZZER_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = STICKC_HW_BEEP_DEFAULT_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGW(TAG, "buzzer timer config failed");
        return;
    }
    ledc_channel_config_t channel = {
        .gpio_num = STICKC_HW_BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = STICKC_HW_BUZZER_CHANNEL,
        .timer_sel = STICKC_HW_BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGW(TAG, "buzzer channel config failed");
    }
}

static void init_led(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << STICKC_HW_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    /* Start with the LED off. */
    gpio_set_level(STICKC_HW_LED_GPIO, STICKC_HW_LED_ACTIVE_LOW ? 1 : 0);
}

static void init_battery(void)
{
    adc_oneshot_unit_init_cfg_t unit = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed; battery.status disabled");
        return;
    }
    adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, STICKC_HW_BATTERY_CHANNEL, &channel) != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel config failed; battery.status disabled");
        return;
    }
    s_adc_ready = true;

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali, &s_adc_cali) == ESP_OK) {
        s_adc_cali_ready = true;
    }
#endif
    ESP_LOGI(TAG, "battery ADC ready");
}

/* Polls both buttons every STICKC_HW_BTN_POLL_MS, debounces (two matching
 * samples), and counts release->press edges. */
static void button_task(void *arg)
{
    (void)arg;
    bool last[STICKC_HW_BTN_COUNT] = {false};
    for (;;) {
        for (int i = 0; i < STICKC_HW_BTN_COUNT; ++i) {
            bool now = gpio_get_level(s_buttons[i].gpio) == 0; /* active low */
            if (now == last[i] && now != s_buttons[i].pressed) {
                s_buttons[i].pressed = now;
                if (now) {
                    s_buttons[i].press_count += 1U;
                }
            }
            last[i] = now;
        }
        vTaskDelay(pdMS_TO_TICKS(STICKC_HW_BTN_POLL_MS));
    }
}

static void init_buttons(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STICKC_HW_BTN_A_GPIO) | (1ULL << STICKC_HW_BTN_B_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGW(TAG, "button GPIO config failed; button.status disabled");
        return;
    }
    if (xTaskCreate(button_task, "stickc_btn", 2560, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "button task create failed; button.status disabled");
        return;
    }
    s_buttons_ready = true;
    ESP_LOGI(TAG, "buttons ready");
}

/* ---------------------------------------------------------------------- */
/*  Command handlers                                                      */
/* ---------------------------------------------------------------------- */

static esp_err_t handle_imu_read(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_json;
    (void)params_len;

    if (!s_imu_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "IMU is not available";
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[14] = {0};
    if (mpu6886_read(MPU6886_REG_ACCEL_XOUT_H, raw, sizeof(raw)) != ESP_OK) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "IMU read failed";
        return ESP_FAIL;
    }

    int16_t ax = (int16_t)((raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)((raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)((raw[4] << 8) | raw[5]);
    int16_t temp_raw = (int16_t)((raw[6] << 8) | raw[7]);
    int16_t gx = (int16_t)((raw[8] << 8) | raw[9]);
    int16_t gy = (int16_t)((raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)((raw[12] << 8) | raw[13]);

    const double accel_lsb = MPU6886_ACCEL_FS_G / 32768.0;
    const double gyro_lsb = MPU6886_GYRO_FS_DPS / 32768.0;

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *accel = cJSON_AddObjectToObject(payload, "accel");
    cJSON_AddNumberToObject(accel, "x", ax * accel_lsb);
    cJSON_AddNumberToObject(accel, "y", ay * accel_lsb);
    cJSON_AddNumberToObject(accel, "z", az * accel_lsb);
    cJSON *gyro = cJSON_AddObjectToObject(payload, "gyro");
    cJSON_AddNumberToObject(gyro, "x", gx * gyro_lsb);
    cJSON_AddNumberToObject(gyro, "y", gy * gyro_lsb);
    cJSON_AddNumberToObject(gyro, "z", gz * gyro_lsb);
    cJSON_AddNumberToObject(payload, "tempC", temp_raw / 326.8 + 25.0);
    cJSON_AddStringToObject(payload, "accelUnit", "g");
    cJSON_AddStringToObject(payload, "gyroUnit", "dps");

    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static esp_err_t handle_buzzer_beep(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_len;

    cJSON *params = NULL;
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_example_parse_json_params(params_json, &params, out_error),
        TAG, "invalid params");

    int frequency = STICKC_HW_BEEP_DEFAULT_HZ;
    int duration_ms = STICKC_HW_BEEP_DEFAULT_MS;
    cJSON *freq_field = cJSON_GetObjectItemCaseSensitive(params, "frequency");
    if (cJSON_IsNumber(freq_field)) {
        frequency = freq_field->valueint;
    }
    cJSON *duration_field = cJSON_GetObjectItemCaseSensitive(params, "durationMs");
    if (cJSON_IsNumber(duration_field)) {
        duration_ms = duration_field->valueint;
    }
    cJSON_Delete(params);

    if (frequency < STICKC_HW_BEEP_MIN_HZ) {
        frequency = STICKC_HW_BEEP_MIN_HZ;
    } else if (frequency > STICKC_HW_BEEP_MAX_HZ) {
        frequency = STICKC_HW_BEEP_MAX_HZ;
    }
    if (duration_ms < 1) {
        duration_ms = 1;
    } else if (duration_ms > STICKC_HW_BEEP_MAX_MS) {
        duration_ms = STICKC_HW_BEEP_MAX_MS;
    }

    esp_err_t err = ledc_set_freq(LEDC_LOW_SPEED_MODE, STICKC_HW_BUZZER_TIMER, (uint32_t)frequency);
    if (err != ESP_OK) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "buzzer is not available";
        return err;
    }
    /* Drive the buzzer for the requested duration, then silence it.  The
     * duration is clamped above so this never blocks the node task long. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, STICKC_HW_BUZZER_CHANNEL, STICKC_HW_BUZZER_DUTY_ON);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, STICKC_HW_BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, STICKC_HW_BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, STICKC_HW_BUZZER_CHANNEL);

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "frequency", frequency);
    cJSON_AddNumberToObject(payload, "durationMs", duration_ms);
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static esp_err_t handle_led_set(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_len;

    cJSON *params = NULL;
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_example_parse_json_params(params_json, &params, out_error),
        TAG, "invalid params");

    cJSON *on_field = cJSON_GetObjectItemCaseSensitive(params, "on");
    if (!cJSON_IsBool(on_field)) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "params must include a boolean field 'on'";
        cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }
    bool on = cJSON_IsTrue(on_field);
    cJSON_Delete(params);

    const bool active_low = STICKC_HW_LED_ACTIVE_LOW;
    gpio_set_level(STICKC_HW_LED_GPIO, (on != active_low) ? 1 : 0);

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(payload, "on", on);
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static esp_err_t handle_battery_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_json;
    (void)params_len;

    if (!s_adc_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "battery ADC is not available";
        return ESP_ERR_INVALID_STATE;
    }

    int raw_sum = 0;
    int samples = 0;
    for (int i = 0; i < STICKC_HW_BATTERY_SAMPLES; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, STICKC_HW_BATTERY_CHANNEL, &raw) == ESP_OK) {
            raw_sum += raw;
            samples += 1;
        }
    }
    if (samples == 0) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "battery ADC read failed";
        return ESP_FAIL;
    }
    int raw_avg = raw_sum / samples;

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "raw", raw_avg);

    if (s_adc_cali_ready) {
        int pin_mv = 0;
        if (adc_cali_raw_to_voltage(s_adc_cali, raw_avg, &pin_mv) == ESP_OK) {
            int battery_mv = (int)(pin_mv * STICKC_HW_BATTERY_DIVIDER);
            int percent = (battery_mv - STICKC_HW_BATTERY_EMPTY_MV) * 100 /
                          (STICKC_HW_BATTERY_FULL_MV - STICKC_HW_BATTERY_EMPTY_MV);
            if (percent < 0) {
                percent = 0;
            } else if (percent > 100) {
                percent = 100;
            }
            cJSON_AddNumberToObject(payload, "millivolts", battery_mv);
            cJSON_AddNumberToObject(payload, "volts", battery_mv / 1000.0);
            cJSON_AddNumberToObject(payload, "percent", percent);
        }
    }
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static esp_err_t handle_button_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_json;
    (void)params_len;

    if (!s_buttons_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "buttons are not available";
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* pressCount is cumulative since boot; the gateway diffs it between polls. */
    static const char *const names[STICKC_HW_BTN_COUNT] = {"a", "b"};
    for (int i = 0; i < STICKC_HW_BTN_COUNT; ++i) {
        cJSON *button = cJSON_AddObjectToObject(payload, names[i]);
        cJSON_AddBoolToObject(button, "pressed", s_buttons[i].pressed);
        cJSON_AddNumberToObject(button, "pressCount", s_buttons[i].press_count);
    }
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

static esp_err_t handle_motion_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    (void)params_json;
    (void)params_len;

    if (!s_imu_ready) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "IMU is not available";
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[14] = {0};
    if (mpu6886_read(MPU6886_REG_ACCEL_XOUT_H, raw, sizeof(raw)) != ESP_OK) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "IMU read failed";
        return ESP_FAIL;
    }
    const double accel_lsb = MPU6886_ACCEL_FS_G / 32768.0;
    const double gyro_lsb = MPU6886_GYRO_FS_DPS / 32768.0;
    double ax = (int16_t)((raw[0] << 8) | raw[1]) * accel_lsb;
    double ay = (int16_t)((raw[2] << 8) | raw[3]) * accel_lsb;
    double az = (int16_t)((raw[4] << 8) | raw[5]) * accel_lsb;
    double gx = (int16_t)((raw[8] << 8) | raw[9]) * gyro_lsb;
    double gy = (int16_t)((raw[10] << 8) | raw[11]) * gyro_lsb;
    double gz = (int16_t)((raw[12] << 8) | raw[13]) * gyro_lsb;

    /* Tilt from the gravity vector; "moving" from the gyro magnitude. */
    double pitch = atan2(-ax, sqrt(ay * ay + az * az)) * STICKC_HW_RAD_TO_DEG;
    double roll = atan2(ay, az) * STICKC_HW_RAD_TO_DEG;
    double gyro_mag = sqrt(gx * gx + gy * gy + gz * gz);

    const char *orientation;
    if (az > 0.7) {
        orientation = "faceUp";
    } else if (az < -0.7) {
        orientation = "faceDown";
    } else if (ax > 0.7 || ax < -0.7) {
        orientation = "onSide";
    } else {
        orientation = "upright";
    }

    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "orientation", orientation);
    cJSON_AddNumberToObject(payload, "pitchDeg", pitch);
    cJSON_AddNumberToObject(payload, "rollDeg", roll);
    cJSON_AddBoolToObject(payload, "moving", gyro_mag > STICKC_HW_MOTION_MOVING_DPS);
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

/* ---------------------------------------------------------------------- */
/*  Registration                                                          */
/* ---------------------------------------------------------------------- */

esp_err_t esp_openclaw_node_stickc_register_hw_node_commands(esp_openclaw_node_handle_t node)
{
    init_imu();
    init_buzzer();
    init_led();
    init_battery();
    init_buttons();

    static const esp_openclaw_node_command_t IMU_READ_COMMAND = {
        .name = "imu.read",
        .handler = handle_imu_read,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t BUZZER_BEEP_COMMAND = {
        .name = "buzzer.beep",
        .handler = handle_buzzer_beep,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t LED_SET_COMMAND = {
        .name = "led.set",
        .handler = handle_led_set,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t BATTERY_STATUS_COMMAND = {
        .name = "battery.status",
        .handler = handle_battery_status,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t BUTTON_STATUS_COMMAND = {
        .name = "button.status",
        .handler = handle_button_status,
        .context = NULL,
    };
    static const esp_openclaw_node_command_t MOTION_STATUS_COMMAND = {
        .name = "motion.status",
        .handler = handle_motion_status,
        .context = NULL,
    };

    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "imu"),
                        TAG, "registering imu capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &IMU_READ_COMMAND),
                        TAG, "registering imu.read failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "buzzer"),
                        TAG, "registering buzzer capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &BUZZER_BEEP_COMMAND),
                        TAG, "registering buzzer.beep failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "led"),
                        TAG, "registering led capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &LED_SET_COMMAND),
                        TAG, "registering led.set failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "battery"),
                        TAG, "registering battery capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &BATTERY_STATUS_COMMAND),
                        TAG, "registering battery.status failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "button"),
                        TAG, "registering button capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &BUTTON_STATUS_COMMAND),
                        TAG, "registering button.status failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "motion"),
                        TAG, "registering motion capability failed");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &MOTION_STATUS_COMMAND),
                        TAG, "registering motion.status failed");
    return ESP_OK;
}
