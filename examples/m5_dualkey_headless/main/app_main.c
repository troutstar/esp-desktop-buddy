/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/command_extensions.h"
#include "esp_desktop_buddy/transport_ble.h"

#include "app_commands.h"
#include "board_hw.h"
#include "example_console.h"
#include "example_app_helpers.h"

#define M5_DUALKEY_CONSOLE_STACK 4096
#define M5_DUALKEY_LED_TASK_STACK 3072

static const char *TAG = "m5_dualkey";
static m5_dualkey_app_t s_app;

static void m5_dualkey_console_print_state(void *ctx, FILE *out)
{
    m5_dualkey_print_state((m5_dualkey_app_t *)ctx, out);
}

static example_console_common_cmds_t s_common_console_cmds = {
    .mutex = NULL,
    .buddy = NULL,
    .transport = NULL,
    .state_cache = NULL,
    .print_state = m5_dualkey_console_print_state,
    .state_ctx = &s_app,
};

static example_console_config_t s_console = {
    .prompt = "buddy> ",
    .banner =
        "M5Stack Chain DualKey Desktop Buddy console\n"
        "Type 'help' to list commands.\n"
        "LHS key rejects the current prompt. RHS key accepts once.\n",
    .common_cmds = &s_common_console_cmds,
    .ctx = &s_app,
    .task_stack_size = M5_DUALKEY_CONSOLE_STACK,
    .task_priority = 4,
};

static esp_desktop_buddy_command_extension_result_t m5_dualkey_refuse_char_begin(
    void *ctx,
    esp_desktop_buddy_t *buddy,
    const esp_desktop_buddy_command_view_t *request)
{
    (void)ctx;
    (void)buddy;
    (void)request;

    return (esp_desktop_buddy_command_extension_result_t){
        .mode = ESP_DESKTOP_BUDDY_COMMAND_EXTENSION_NO_ACK,
    };
}

static const esp_desktop_buddy_command_extension_entry_t s_command_extension_bindings[] = {
    { .command = "char_begin", .handler = m5_dualkey_refuse_char_begin },
};

static void m5_dualkey_mark_prompt_answered(m5_dualkey_app_t *app)
{
    xSemaphoreTake(app->mutex, portMAX_DELAY);
    app->state_cache.prompt.present = false;
    app->state_cache.prompt.id[0] = '\0';
    app->state_cache.prompt.tool[0] = '\0';
    app->state_cache.prompt.hint[0] = '\0';
    xSemaphoreGive(app->mutex);
}

static void m5_dualkey_button_cb(uint32_t input_index, bool pressed, void *user_data)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)user_data;
    esp_desktop_buddy_permission_decision_t decision;
    const char *label;
    esp_err_t err;

    if (app == NULL) {
        return;
    }

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    if (input_index == M5_DUALKEY_LEFT_INPUT_INDEX) {
        app->left_key_pressed = pressed;
    } else if (input_index == M5_DUALKEY_RIGHT_INPUT_INDEX) {
        app->right_key_pressed = pressed;
    }
    xSemaphoreGive(app->mutex);

    if (!pressed) {
        return;
    }

    if (input_index == M5_DUALKEY_LEFT_INPUT_INDEX) {
        decision = ESP_DESKTOP_BUDDY_PERMISSION_DECISION_DENY;
        label = "lhs reject";
    } else if (input_index == M5_DUALKEY_RIGHT_INPUT_INDEX) {
        decision = ESP_DESKTOP_BUDDY_PERMISSION_DECISION_ONCE;
        label = "rhs accept";
    } else {
        return;
    }

    err = example_reply_current_prompt(app->mutex,
                                         app->buddy,
                                         app->transport,
                                         &app->state_cache,
                                         decision);
    if (err == ESP_OK) {
        m5_dualkey_mark_prompt_answered(app);
    }
    ESP_LOGI(TAG, "%s rc=%s", label, esp_err_to_name(err));
}

static void m5_dualkey_led_task(void *ctx)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;
    uint32_t prev_left = UINT32_MAX;
    uint32_t prev_right = UINT32_MAX;

    while (true) {
        example_buddy_state_cache_t state_cache = {0};
        esp_desktop_buddy_transport_ble_state_t transport_state = {0};
        bool have_state;
        uint32_t left = 0;
        uint32_t right = 0;
        bool blink_on;
        TickType_t now = xTaskGetTickCount();

        xSemaphoreTake(app->mutex, portMAX_DELAY);
        state_cache = app->state_cache;
        transport_state = app->transport_state;
        have_state = app->have_state;
        xSemaphoreGive(app->mutex);

        if (have_state && state_cache.prompt.present) {
            blink_on = ((now / pdMS_TO_TICKS(200)) & 1u) == 0;
            left = blink_on ? 0x300000 : 0x080000;
            right = blink_on ? 0x003000 : 0x000800;
        } else if (!transport_state.connected || !transport_state.encrypted) {
            /*
             * Pulse whenever the board is advertising or a BLE link is still being secured.
             * This keeps the visual cue independent of the pairing method.
             */
            blink_on = ((now / pdMS_TO_TICKS(350)) & 1u) == 0;
            left = right = blink_on ? 0x001018 : 0x000000;
        } else if (transport_state.connected) {
            /* Stay dark outside attention states to avoid wasting battery. */
            left = right = 0x000000;
        }

        if (left != prev_left || right != prev_right) {
            ESP_ERROR_CHECK(m5_dualkey_board_set_leds(&app->board, left, right));
            prev_left = left;
            prev_right = right;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    esp_desktop_buddy_config_t buddy_config = {
        .event_sink = {
            .on_event = m5_dualkey_buddy_event,
            .ctx = &s_app,
        },
        .handlers = {
            .ctx = &s_app,
            .on_status = m5_dualkey_status_handler,
            .on_name = m5_dualkey_name_handler,
            .on_owner = m5_dualkey_owner_handler,
            .on_unpair = m5_dualkey_unpair_handler,
            .command_extension = {
                .bindings = s_command_extension_bindings,
                .binding_count = sizeof(s_command_extension_bindings) /
                                 sizeof(s_command_extension_bindings[0]),
            },
        },
    };
    esp_desktop_buddy_transport_ble_config_t transport_config = {
        .advertising_name_override = NULL,
        .security = {
            .bonding = true,
            .mitm = false,
            .secure_connections = true,
            .io_capability = ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_NO_INPUT_OUTPUT,
        },
        .on_event = m5_dualkey_transport_event,
        .event_ctx = &s_app,
    };

    ESP_ERROR_CHECK(example_init_nvs());

    m5_dualkey_app_init(&s_app);
    s_app.mutex = xSemaphoreCreateMutex();
    if (s_app.mutex == NULL) {
        abort();
    }

    example_restore_persisted_string("buddy",
                                       "display_name",
                                       s_app.display_name,
                                       sizeof(s_app.display_name),
                                       "DualKey Buddy");

    ESP_ERROR_CHECK(esp_desktop_buddy_new(&buddy_config, &s_app.buddy));
    transport_config.buddy = s_app.buddy;
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_new(&transport_config, &s_app.transport));

    s_common_console_cmds.mutex = s_app.mutex;
    s_common_console_cmds.buddy = s_app.buddy;
    s_common_console_cmds.transport = s_app.transport;
    s_common_console_cmds.state_cache = &s_app.state_cache;

    ESP_ERROR_CHECK(m5_dualkey_board_init(&s_app.board, m5_dualkey_button_cb, &s_app));
    if (xTaskCreate(m5_dualkey_led_task,
                    "dualkey_leds",
                    M5_DUALKEY_LED_TASK_STACK,
                    &s_app,
                    4,
                    NULL) != pdPASS) {
        abort();
    }

    example_console_init();
    ESP_ERROR_CHECK(example_console_start(&s_console));

    ESP_LOGI(TAG, "ready: M5Stack Chain DualKey Desktop Buddy example");
}
