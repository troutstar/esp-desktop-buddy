#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/command_extensions.h"
#include "esp_desktop_buddy/transport_ble.h"
#include "example_app_helpers.h"

#include "app_commands.h"
#include "cyd_display.h"
#include "cyd_touch.h"
#include "cyd_ui.h"

#define TAG "cyd_main"

static cyd_app_t s_app;

/* Refuse char_begin — we have no charpack support */
static esp_desktop_buddy_command_extension_result_t refuse_char_begin(
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

static const esp_desktop_buddy_command_extension_entry_t s_ext_bindings[] = {
    {.command = "char_begin", .handler = refuse_char_begin},
};

void app_main(void)
{
    esp_desktop_buddy_config_t buddy_config = {
        .event_sink = {
            .on_event = cyd_buddy_event,
            .ctx = &s_app,
        },
        .handlers = {
            .ctx = &s_app,
            .on_status = cyd_status_handler,
            .on_name = cyd_name_handler,
            .on_owner = cyd_owner_handler,
            .on_unpair = cyd_unpair_handler,
            .command_extension = {
                .bindings = s_ext_bindings,
                .binding_count = sizeof(s_ext_bindings) / sizeof(s_ext_bindings[0]),
            },
        },
    };
    esp_desktop_buddy_transport_ble_config_t transport_config = {
        .security = {
            .bonding = true,
            .mitm = true,
            .secure_connections = true,
            .io_capability = ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY,
        },
        .on_event = cyd_transport_event,
        .event_ctx = &s_app,
    };

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    ESP_ERROR_CHECK(example_init_nvs());

    cyd_app_init(&s_app);
    s_app.mutex = xSemaphoreCreateMutex();
    if (s_app.mutex == NULL) {
        abort();
    }

    example_restore_persisted_string("buddy", "display_name",
                                     s_app.display_name, sizeof(s_app.display_name),
                                     "CYD Buddy");

    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_init());

    ESP_ERROR_CHECK(esp_desktop_buddy_new(&buddy_config, &s_app.buddy));
    transport_config.buddy = s_app.buddy;
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_new(&transport_config, &s_app.transport));
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_get_advertising_name(
        s_app.transport, s_app.advertising_name, sizeof(s_app.advertising_name)));

    ESP_ERROR_CHECK(cyd_ui_init(&s_app));

    ESP_LOGI(TAG, "ready: CYD Desktop Buddy (advertising as %s)", s_app.advertising_name);
}
