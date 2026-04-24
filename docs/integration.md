# Integration

Use these components:

- [`esp_desktop_buddy`](../components/esp_desktop_buddy/README.md): protocol core
- [`esp_desktop_buddy_transport_ble`](../components/esp_desktop_buddy_transport_ble/README.md): BLE transport
- [`esp_desktop_buddy_folder_push`](../components/esp_desktop_buddy_folder_push/README.md): optional `char_*` support

## Add Dependencies

Most apps need the core and BLE transport:

```cmake
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_desktop_buddy
        esp_desktop_buddy_transport_ble
)
```

Add `esp_desktop_buddy_folder_push` only if your device accepts pushed files.

## Minimal Example

```c
#include "esp_check.h"
#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_ble.h"

static esp_desktop_buddy_status_reply_t app_status(void *ctx, esp_desktop_buddy_t *buddy)
{
    static const char status_json[] =
        "{"
        "\"name\":\"Buddy\","
        "\"owner\":\"\","
        "\"sec\":true,"
        "\"sys\":{\"up\":0,\"heap\":0},"
        "\"stats\":{\"appr\":0,\"deny\":0}"
        "}";

    // Return the JSON object that becomes status.data.
    return esp_desktop_buddy_status_ok((esp_desktop_buddy_json_object_view_t){
        .bytes = status_json,
        .len = sizeof(status_json) - 1,
    });
}

static esp_desktop_buddy_command_result_t app_name(void *ctx,
                                                   esp_desktop_buddy_t *buddy,
                                                   const char *name)
{
    // Persist or apply the new display name here.
    return esp_desktop_buddy_command_ok();
}

void app_main(void)
{
    esp_desktop_buddy_t *buddy = NULL;
    esp_desktop_buddy_transport_ble_t *transport = NULL;
    esp_desktop_buddy_config_t buddy_cfg = {
        .handlers = {
            .on_status = app_status,
            .on_name = app_name,
        },
    };

    // Create the Buddy core first.
    ESP_ERROR_CHECK(esp_desktop_buddy_new(&buddy_cfg, &buddy));

    // Then attach the BLE transport to that core.
    ESP_ERROR_CHECK(esp_desktop_buddy_transport_ble_new(
        &(esp_desktop_buddy_transport_ble_config_t){
            .buddy = buddy,
            .security = {
                .bonding = true,
                .mitm = true,
                .secure_connections = true,
                .io_capability = ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY,
            },
        },
        &transport));
}
```

## Your App Owns

- `status.data`
- `name`, `owner`, and `unpair` behavior
- storage, UI, and board policy
- optional folder-push sink behavior

## Notes

- create the core before the BLE transport
- destroy the BLE transport before the core
- BLE advertising name and Buddy display name are separate
- `status.data` is application-defined
- folder push is optional
- use `examples/generic_headless` as the starting point
- run [testing.md](testing.md) before release
