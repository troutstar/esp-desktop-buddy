idf_component_register(
    SRCS
        "app_main.c"
        "app_commands.c"
        "generic_headless_led.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_desktop_buddy
        esp_desktop_buddy_transport_ble
        example_console
        example_app_helpers
        example_status
        espressif__cjson
    PRIV_REQUIRES
        esp_driver_gpio
        led_strip
)
