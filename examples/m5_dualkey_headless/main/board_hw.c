/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_hw.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/task.h"

static const char *TAG = "m5_dualkey_hw";

#define M5_DUALKEY_ACTIVE_LEVEL 0
#define M5_DUALKEY_DEBOUNCE_TICKS 3
#define M5_DUALKEY_TICKS_INTERVAL_US 1000
#define M5_DUALKEY_LEFT_GPIO 0
#define M5_DUALKEY_RIGHT_GPIO 17
#define M5_DUALKEY_WS2812_POWER_GPIO 40
#define M5_DUALKEY_WS2812_GPIO 21
#define M5_DUALKEY_LED_COUNT 2
#define M5_DUALKEY_BATTERY_ADC_CHANNEL ADC_CHANNEL_9
#define M5_DUALKEY_CHARGE_ADC_CHANNEL ADC_CHANNEL_8
#define M5_DUALKEY_VBUS_ADC_CHANNEL ADC_CHANNEL_1
#define M5_DUALKEY_ADC_ATTEN ADC_ATTEN_DB_12
#define M5_DUALKEY_ADC_SAMPLE_COUNT 16
#define M5_DUALKEY_ADC_VREF_MV 3333.0f
#define M5_DUALKEY_ADC_MAX_READING 4095.0f
#define M5_DUALKEY_VOLTAGE_DIVIDER_RATIO 1.51f
#define M5_DUALKEY_VBUS_PRESENT_MV 4000
#define M5_DUALKEY_CHARGE_NOT_CHARGING_MV 3000
#define M5_DUALKEY_CHARGE_FULL_MV 1900
#define M5_DUALKEY_CHARGE_ACTIVE_MV 1400
#define M5_DUALKEY_BUTTON_POLL_MS 10
#define M5_DUALKEY_BUTTON_DEBOUNCE_SAMPLES 3

static const int s_input_gpios[] = {
    M5_DUALKEY_LEFT_GPIO,
    M5_DUALKEY_RIGHT_GPIO,
};

typedef struct {
    uint16_t mv;
    uint8_t pct;
} m5_dualkey_voltage_point_t;

typedef struct {
    bool sampled_pressed;
    bool stable_pressed;
    uint8_t stable_count;
} m5_dualkey_button_state_t;

static const m5_dualkey_voltage_point_t s_charge_curve[] = {
    {3400, 0},
    {3610, 25},
    {3880, 50},
    {4120, 75},
    {4200, 100},
};

static const m5_dualkey_voltage_point_t s_discharge_curve[] = {
    {3330, 0},
    {3550, 25},
    {3810, 50},
    {4070, 75},
    {4200, 100},
};

static esp_err_t m5_dualkey_board_power_leds(bool enable)
{
    ESP_RETURN_ON_ERROR(gpio_hold_dis(M5_DUALKEY_WS2812_POWER_GPIO), TAG, "hold disable failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(M5_DUALKEY_WS2812_POWER_GPIO, enable ? 0 : 1),
                        TAG,
                        "power set failed");
    if (enable) {
        ESP_RETURN_ON_ERROR(gpio_hold_en(M5_DUALKEY_WS2812_POWER_GPIO),
                            TAG,
                            "hold enable failed");
    }
    return ESP_OK;
}

static esp_err_t m5_dualkey_board_init_leds(m5_dualkey_board_t *board)
{
    gpio_config_t power_cfg = {
        .pin_bit_mask = 1ULL << M5_DUALKEY_WS2812_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    led_strip_config_t strip_config = {
        .strip_gpio_num = M5_DUALKEY_WS2812_GPIO,
        .max_leds = M5_DUALKEY_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = SPI2_HOST,
        .flags = {
            .with_dma = true,
        },
    };

    ESP_RETURN_ON_ERROR(gpio_config(&power_cfg), TAG, "power gpio init failed");
    ESP_RETURN_ON_ERROR(m5_dualkey_board_power_leds(false), TAG, "led power off failed");
    ESP_RETURN_ON_ERROR(led_strip_new_spi_device(&strip_config, &spi_config, &board->led_strip),
                        TAG,
                        "led strip init failed");
    ESP_RETURN_ON_ERROR(m5_dualkey_board_power_leds(true), TAG, "led power on failed");
    return m5_dualkey_board_clear(board);
}

static esp_err_t m5_dualkey_board_init_power(m5_dualkey_board_t *board)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_chan_cfg_t channel_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = M5_DUALKEY_ADC_ATTEN,
    };

    if (board->adc_unit != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_cfg, &board->adc_unit),
                        TAG,
                        "adc unit init failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(board->adc_unit,
                                                   M5_DUALKEY_BATTERY_ADC_CHANNEL,
                                                   &channel_cfg),
                        TAG,
                        "battery adc init failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(board->adc_unit,
                                                   M5_DUALKEY_CHARGE_ADC_CHANNEL,
                                                   &channel_cfg),
                        TAG,
                        "charge adc init failed");
    return adc_oneshot_config_channel(board->adc_unit,
                                      M5_DUALKEY_VBUS_ADC_CHANNEL,
                                      &channel_cfg);
}

static void m5_dualkey_button_task(void *ctx)
{
    m5_dualkey_board_t *board = (m5_dualkey_board_t *)ctx;
    m5_dualkey_button_state_t button_state[sizeof(s_input_gpios) / sizeof(s_input_gpios[0])];

    for (size_t i = 0; i < sizeof(s_input_gpios) / sizeof(s_input_gpios[0]); ++i) {
        bool pressed = gpio_get_level(s_input_gpios[i]) == M5_DUALKEY_ACTIVE_LEVEL;

        button_state[i].sampled_pressed = pressed;
        button_state[i].stable_pressed = pressed;
        button_state[i].stable_count = M5_DUALKEY_BUTTON_DEBOUNCE_SAMPLES;
    }

    while (true) {
        for (size_t i = 0; i < sizeof(s_input_gpios) / sizeof(s_input_gpios[0]); ++i) {
            bool pressed = gpio_get_level(s_input_gpios[i]) == M5_DUALKEY_ACTIVE_LEVEL;

            if (pressed == button_state[i].sampled_pressed) {
                if (button_state[i].stable_count < M5_DUALKEY_BUTTON_DEBOUNCE_SAMPLES) {
                    button_state[i].stable_count++;
                }
            } else {
                button_state[i].sampled_pressed = pressed;
                button_state[i].stable_count = 1;
            }

            if (button_state[i].stable_count == M5_DUALKEY_BUTTON_DEBOUNCE_SAMPLES &&
                button_state[i].stable_pressed != pressed) {
                button_state[i].stable_pressed = pressed;
                if (board->button_cb != NULL) {
                    board->button_cb((uint32_t)i, pressed, board->button_cb_ctx);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(M5_DUALKEY_BUTTON_POLL_MS));
    }
}

static esp_err_t m5_dualkey_board_init_keyboard(m5_dualkey_board_t *board,
                                                m5_dualkey_button_callback_t button_cb,
                                                void *button_cb_ctx)
{
    gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << M5_DUALKEY_LEFT_GPIO) | (1ULL << M5_DUALKEY_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "keyboard gpio init failed");

    board->button_cb = button_cb;
    board->button_cb_ctx = button_cb_ctx;

    if (xTaskCreate(m5_dualkey_button_task,
                    "dualkey_keys",
                    3072,
                    board,
                    5,
                    &board->button_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void m5_dualkey_set_rgb(led_strip_handle_t led_strip, uint32_t index, uint32_t rgb)
{
    uint8_t red = (rgb >> 16) & 0xff;
    uint8_t green = (rgb >> 8) & 0xff;
    uint8_t blue = rgb & 0xff;

    led_strip_set_pixel(led_strip, index, red, green, blue);
}

static esp_err_t m5_dualkey_board_read_average(adc_oneshot_unit_handle_t adc_unit,
                                               adc_channel_t channel,
                                               uint32_t *average)
{
    int reading = 0;
    uint32_t sum = 0;

    for (uint32_t i = 0; i < M5_DUALKEY_ADC_SAMPLE_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(adc_unit, channel, &reading),
                            TAG,
                            "adc read failed");
        sum += (uint32_t)reading;
    }

    *average = sum / M5_DUALKEY_ADC_SAMPLE_COUNT;
    return ESP_OK;
}

static uint16_t m5_dualkey_board_adc_to_mv(uint32_t reading, float scale)
{
    float raw_mv = ((float)reading * M5_DUALKEY_ADC_VREF_MV) / M5_DUALKEY_ADC_MAX_READING;

    return (uint16_t)(raw_mv * scale);
}

static uint8_t m5_dualkey_board_interpolate_pct(uint16_t battery_mv, bool charging)
{
    const m5_dualkey_voltage_point_t *curve =
        charging ? s_charge_curve : s_discharge_curve;
    size_t curve_len = charging
        ? (sizeof(s_charge_curve) / sizeof(s_charge_curve[0]))
        : (sizeof(s_discharge_curve) / sizeof(s_discharge_curve[0]));

    if (battery_mv <= curve[0].mv) {
        return 0;
    }
    if (battery_mv >= curve[curve_len - 1].mv) {
        return 100;
    }

    for (size_t i = 0; i + 1 < curve_len; ++i) {
        const m5_dualkey_voltage_point_t *lower = &curve[i];
        const m5_dualkey_voltage_point_t *upper = &curve[i + 1];

        if (battery_mv >= lower->mv && battery_mv < upper->mv) {
            uint32_t mv_range = upper->mv - lower->mv;
            uint32_t pct_range = upper->pct - lower->pct;
            uint32_t mv_offset = battery_mv - lower->mv;

            return (uint8_t)(lower->pct + ((mv_offset * pct_range) / mv_range));
        }
    }

    return 100;
}

esp_err_t m5_dualkey_board_init(m5_dualkey_board_t *board,
                                m5_dualkey_button_callback_t button_cb,
                                void *button_cb_ctx)
{
    ESP_RETURN_ON_FALSE(board != NULL, ESP_ERR_INVALID_ARG, TAG, "board is null");

    board->led_strip = NULL;
    board->button_task = NULL;
    board->button_cb = NULL;
    board->button_cb_ctx = NULL;
    board->adc_unit = NULL;

    ESP_RETURN_ON_ERROR(m5_dualkey_board_init_leds(board), TAG, "board led init failed");
    ESP_RETURN_ON_ERROR(m5_dualkey_board_init_power(board), TAG, "board power init failed");
    return m5_dualkey_board_init_keyboard(board, button_cb, button_cb_ctx);
}

esp_err_t m5_dualkey_board_set_leds(m5_dualkey_board_t *board,
                                    uint32_t left_rgb,
                                    uint32_t right_rgb)
{
    ESP_RETURN_ON_FALSE(board != NULL && board->led_strip != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "board not initialized");

    m5_dualkey_set_rgb(board->led_strip, M5_DUALKEY_LEFT_LED_INDEX, left_rgb);
    m5_dualkey_set_rgb(board->led_strip, M5_DUALKEY_RIGHT_LED_INDEX, right_rgb);
    return led_strip_refresh(board->led_strip);
}

esp_err_t m5_dualkey_board_clear(m5_dualkey_board_t *board)
{
    return m5_dualkey_board_set_leds(board, 0, 0);
}

esp_err_t m5_dualkey_board_get_power_state(m5_dualkey_board_t *board,
                                           m5_dualkey_board_power_state_t *power_state)
{
    uint32_t battery_average = 0;
    uint32_t charge_average = 0;
    uint32_t vbus_average = 0;
    uint16_t battery_mv;
    uint16_t charge_mv;
    uint16_t vbus_mv;
    bool usb_present;
    bool charging;
    uint8_t battery_pct;

    ESP_RETURN_ON_FALSE(board != NULL && power_state != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid power state request");
    ESP_RETURN_ON_FALSE(board->adc_unit != NULL, ESP_ERR_INVALID_STATE, TAG, "adc not initialized");

    ESP_RETURN_ON_ERROR(m5_dualkey_board_read_average(board->adc_unit,
                                                      M5_DUALKEY_BATTERY_ADC_CHANNEL,
                                                      &battery_average),
                        TAG,
                        "battery read failed");
    ESP_RETURN_ON_ERROR(m5_dualkey_board_read_average(board->adc_unit,
                                                      M5_DUALKEY_CHARGE_ADC_CHANNEL,
                                                      &charge_average),
                        TAG,
                        "charge read failed");
    ESP_RETURN_ON_ERROR(m5_dualkey_board_read_average(board->adc_unit,
                                                      M5_DUALKEY_VBUS_ADC_CHANNEL,
                                                      &vbus_average),
                        TAG,
                        "vbus read failed");

    battery_mv = m5_dualkey_board_adc_to_mv(battery_average, M5_DUALKEY_VOLTAGE_DIVIDER_RATIO);
    charge_mv = m5_dualkey_board_adc_to_mv(charge_average, 1.0f);
    vbus_mv = m5_dualkey_board_adc_to_mv(vbus_average, M5_DUALKEY_VOLTAGE_DIVIDER_RATIO);

    usb_present = vbus_mv > M5_DUALKEY_VBUS_PRESENT_MV;
    charging = charge_mv > M5_DUALKEY_CHARGE_ACTIVE_MV &&
               charge_mv <= M5_DUALKEY_CHARGE_FULL_MV;
    if (charge_mv > M5_DUALKEY_CHARGE_FULL_MV &&
        charge_mv <= M5_DUALKEY_CHARGE_NOT_CHARGING_MV) {
        battery_pct = 100;
    } else {
        battery_pct = m5_dualkey_board_interpolate_pct(battery_mv, charging);
    }

    power_state->battery_mv = battery_mv;
    power_state->battery_pct = battery_pct;
    power_state->usb_present = usb_present;
    power_state->charging = charging;
    return ESP_OK;
}
