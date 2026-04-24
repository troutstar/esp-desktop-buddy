/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

#include "generic_headless_led.h"

#define BLINK_GPIO CONFIG_BLINK_GPIO
#define GENERIC_HEADLESS_LED_TASK_STACK 3072

static const char *TAG = "generic_led";

#ifdef CONFIG_BLINK_LED_STRIP
static led_strip_handle_t s_led_strip;
#endif

static bool generic_headless_should_blink_pairing(
    const esp_desktop_buddy_transport_ble_state_t *transport_state)
{
    if (transport_state == NULL) {
        return false;
    }

    return transport_state->has_passkey || (transport_state->connected && !transport_state->encrypted);
}

static bool generic_headless_blink_phase(TickType_t now, uint32_t period_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(period_ms);

    if (ticks == 0) {
        ticks = 1;
    }

    return ((now / ticks) & 1u) == 0;
}

#ifdef CONFIG_BLINK_LED_STRIP
static void generic_headless_set_led(bool on)
{
    if (on) {
        led_strip_set_pixel(s_led_strip, 0, 16, 16, 16);
        led_strip_refresh(s_led_strip);
    } else {
        led_strip_clear(s_led_strip);
    }
}

static esp_err_t generic_headless_configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };

    ESP_LOGI(TAG, "configured addressable LED on GPIO%d", BLINK_GPIO);
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip),
                        TAG,
                        "led strip init failed");
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_spi_device(&strip_config, &spi_config, &s_led_strip),
                        TAG,
                        "led strip init failed");
#else
#error "unsupported LED strip backend"
#endif
    led_strip_clear(s_led_strip);
    return ESP_OK;
}
#elif CONFIG_BLINK_LED_GPIO
static void generic_headless_set_led(bool on)
{
    gpio_set_level(BLINK_GPIO, on);
}

static esp_err_t generic_headless_configure_led(void)
{
    ESP_LOGI(TAG, "configured GPIO LED on GPIO%d", BLINK_GPIO);
    ESP_RETURN_ON_ERROR(gpio_reset_pin(BLINK_GPIO), TAG, "gpio reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT), TAG, "gpio output failed");
    generic_headless_set_led(false);
    return ESP_OK;
}
#else
#error "unsupported LED type"
#endif

static void generic_headless_led_task(void *ctx)
{
    generic_headless_app_t *app = (generic_headless_app_t *)ctx;
    bool previous_on = false;
    bool previous_valid = false;

    while (true) {
        example_buddy_state_cache_t state_cache = {0};
        esp_desktop_buddy_transport_ble_state_t transport_state = {0};
        bool have_state = false;
        bool led_on = false;
        TickType_t now = xTaskGetTickCount();

        xSemaphoreTake(app->mutex, portMAX_DELAY);
        state_cache = app->state_cache;
        transport_state = app->transport_state;
        have_state = app->have_state;
        xSemaphoreGive(app->mutex);

        if (have_state && state_cache.prompt.present) {
            uint32_t prompt_period_ms = CONFIG_BLINK_PERIOD / 2U;

            if (prompt_period_ms < 100U) {
                prompt_period_ms = 100U;
            }
            led_on = generic_headless_blink_phase(now, prompt_period_ms);
        } else if (generic_headless_should_blink_pairing(&transport_state)) {
            led_on = generic_headless_blink_phase(now, CONFIG_BLINK_PERIOD);
        }

        if (!previous_valid || previous_on != led_on) {
            generic_headless_set_led(led_on);
            previous_on = led_on;
            previous_valid = true;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t generic_headless_led_init(generic_headless_app_t *app)
{
    if (app == NULL || app->mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(generic_headless_configure_led(), TAG, "led configure failed");

    if (xTaskCreate(generic_headless_led_task,
                    "generic_led",
                    GENERIC_HEADLESS_LED_TASK_STACK,
                    app,
                    4,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
