/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define M5_DUALKEY_LEFT_INPUT_INDEX 0
#define M5_DUALKEY_RIGHT_INPUT_INDEX 1
#define M5_DUALKEY_LEFT_LED_INDEX 1
#define M5_DUALKEY_RIGHT_LED_INDEX 0

typedef struct {
    uint16_t battery_mv;
    uint8_t battery_pct;
    bool usb_present;
    bool charging;
} m5_dualkey_board_power_state_t;

typedef void (*m5_dualkey_button_callback_t)(uint32_t input_index, bool pressed, void *user_data);

typedef struct {
    led_strip_handle_t led_strip;
    TaskHandle_t button_task;
    m5_dualkey_button_callback_t button_cb;
    void *button_cb_ctx;
    adc_oneshot_unit_handle_t adc_unit;
} m5_dualkey_board_t;

esp_err_t m5_dualkey_board_init(m5_dualkey_board_t *board,
                                m5_dualkey_button_callback_t button_cb,
                                void *button_cb_ctx);
esp_err_t m5_dualkey_board_set_leds(m5_dualkey_board_t *board,
                                    uint32_t left_rgb,
                                    uint32_t right_rgb);
esp_err_t m5_dualkey_board_clear(m5_dualkey_board_t *board);
esp_err_t m5_dualkey_board_get_power_state(m5_dualkey_board_t *board,
                                           m5_dualkey_board_power_state_t *power_state);
