idf_component_register(
    SRCS
        "app_main.c"
        "app_commands.c"
        "ui.c"
        "charpack_console.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_desktop_buddy
        esp_desktop_buddy_transport_ble
        esp_desktop_buddy_folder_push
        example_console
        example_app_helpers
        example_status
        example_charpack
        espressif__cjson
        esp-box-3
)
