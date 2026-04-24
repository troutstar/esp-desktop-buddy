/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "example_console.h"
#include "example_app_helpers.h"

#include "app_shared.h"

static const char *TAG = "esp_box_3_demo";
static box_demo_app_t s_app;

void app_main(void)
{
    const esp_desktop_buddy_folder_push_sink_t *sink;
    esp_desktop_buddy_config_t buddy_config = {
        .event_sink = {
            .on_event = box_demo_buddy_event,
            .ctx = &s_app,
        },
        .handlers = {
            .ctx = &s_app,
            .on_status = box_demo_status_handler,
            .on_name = box_demo_name_handler,
            .on_owner = box_demo_owner_handler,
            .on_unpair = box_demo_unpair_handler,
        },
    };
    esp_desktop_buddy_transport_ble_config_t transport_config = {
        .security = {
            .bonding = true,
            .mitm = true,
            .secure_connections = true,
            .io_capability = ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY,
        },
        .on_event = box_demo_transport_event,
        .event_ctx = &s_app,
    };
    example_charpack_config_t charpack_config = {
        .mount_point = NULL,
        .packs_root = NULL,
        .staging_root = NULL,
        .format_if_mount_failed = true,
        .on_event = box_demo_charpack_event,
        .event_ctx = &s_app,
    };
    esp_desktop_buddy_folder_push_config_t folder_push_config = {0};

    ESP_ERROR_CHECK(example_init_nvs());

    box_demo_app_init(&s_app);
    s_app.mutex = xSemaphoreCreateMutex();
    if (s_app.mutex == NULL) {
        abort();
    }

    example_restore_persisted_string("buddy",
                                       "display_name",
                                       s_app.display_name,
                                       sizeof(s_app.display_name),
                                       "Box Buddy");

    ESP_ERROR_CHECK(example_charpack_new(&charpack_config, &s_app.charpack));
    if (example_charpack_get_active(s_app.charpack, &s_app.active_pack) == ESP_OK) {
        s_app.have_active_pack = true;
        example_safe_copy(s_app.pack_status,
                            sizeof(s_app.pack_status),
                            "Active pack loaded");
    }

    sink = example_charpack_get_sink(s_app.charpack);
    folder_push_config.sink = *sink;
    ESP_ERROR_CHECK(esp_desktop_buddy_folder_push_new(&folder_push_config, &s_app.folder_push));

    buddy_config.handlers.command_extension = esp_desktop_buddy_folder_push_command_extension(s_app.folder_push);
    ESP_ERROR_CHECK(esp_desktop_buddy_new(&buddy_config, &s_app.buddy));
    transport_config.buddy = s_app.buddy;
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_new(&transport_config, &s_app.transport));
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_get_advertising_name(s_app.transport,
                                                                         s_app.advertising_name,
                                                                         sizeof(s_app.advertising_name)));

    example_console_init();
    ESP_ERROR_CHECK(box_demo_ui_init(&s_app));
    box_demo_ui_start(&s_app);
    box_demo_charpack_console_start(&s_app);

    ESP_LOGI(TAG,
             "ready: ESP-BOX-3 Desktop Buddy demo (advertising as %s)",
             s_app.advertising_name);
}
