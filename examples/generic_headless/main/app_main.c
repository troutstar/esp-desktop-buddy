/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/command_extensions.h"
#include "esp_desktop_buddy/transport_ble.h"

#include "app_commands.h"
#include "example_console.h"
#include "example_app_helpers.h"
#include "generic_headless_led.h"

#define GENERIC_HEADLESS_CONSOLE_STACK 4096

static const char *TAG = "generic_headless";
static generic_headless_app_t s_app;

static void generic_headless_console_print_state(void *ctx, FILE *out)
{
    generic_headless_print_state((generic_headless_app_t *)ctx, out);
}

static example_console_common_cmds_t s_common_console_cmds = {
    .mutex = NULL,
    .buddy = NULL,
    .transport = NULL,
    .state_cache = NULL,
    .print_state = generic_headless_console_print_state,
    .state_ctx = &s_app,
};

static example_console_config_t s_console = {
    .prompt = "buddy> ",
    .banner =
        "Desktop Buddy generic headless console\n"
        "Type 'help' to list commands.\n",
    .common_cmds = &s_common_console_cmds,
    .ctx = &s_app,
    .task_stack_size = GENERIC_HEADLESS_CONSOLE_STACK,
    .task_priority = 4,
};

static esp_desktop_buddy_command_extension_result_t generic_headless_refuse_char_begin(
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
    { .command = "char_begin", .handler = generic_headless_refuse_char_begin },
};

void app_main(void)
{
    esp_desktop_buddy_config_t buddy_config = {
        .event_sink = {
            .on_event = generic_headless_buddy_event,
            .ctx = &s_app,
        },
        .handlers = {
            .ctx = &s_app,
            .on_status = generic_headless_status_handler,
            .on_name = generic_headless_name_handler,
            .on_owner = generic_headless_owner_handler,
            .on_unpair = generic_headless_unpair_handler,
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
            .mitm = true,
            .secure_connections = true,
            .io_capability = ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY,
        },
        .on_event = generic_headless_transport_event,
        .event_ctx = &s_app,
    };

    ESP_ERROR_CHECK(example_init_nvs());

    generic_headless_app_init(&s_app);
    s_app.mutex = xSemaphoreCreateMutex();
    if (s_app.mutex == NULL) {
        abort();
    }
    ESP_ERROR_CHECK(generic_headless_led_init(&s_app));

    example_restore_persisted_string("buddy",
                                       "display_name",
                                       s_app.display_name,
                                       sizeof(s_app.display_name),
                                       "Buddy");

    ESP_ERROR_CHECK(esp_desktop_buddy_new(&buddy_config, &s_app.buddy));
    transport_config.buddy = s_app.buddy;
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_new(&transport_config, &s_app.transport));

    s_common_console_cmds.mutex = s_app.mutex;
    s_common_console_cmds.buddy = s_app.buddy;
    s_common_console_cmds.transport = s_app.transport;
    s_common_console_cmds.state_cache = &s_app.state_cache;

    example_console_init();
    ESP_ERROR_CHECK(example_console_start(&s_console));

    ESP_LOGI(TAG, "ready: generic Desktop Buddy headless example");
}
