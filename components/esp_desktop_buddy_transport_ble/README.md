# esp_desktop_buddy_transport_ble

NimBLE BLE transport for `esp_desktop_buddy`. It handles advertising, pairing, bonding, and encrypted TX.
The transport supports one active BLE peer at a time and stops advertising while that peer is connected.

Public header: `include/esp_desktop_buddy/transport_ble.h`

Dependency:

```cmake
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_desktop_buddy esp_desktop_buddy_transport_ble
)
```
