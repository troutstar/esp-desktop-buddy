# Examples

These examples show different ways to build a Buddy device on top of the public SDK components.

- `generic_headless/`: the smallest starting point. It uses the Buddy core and BLE transport, exposes a serial console, and does not include board-specific UI or `char_*` support.
- `m5_dualkey_headless/`: a simple hardware-backed example for the [M5Stack Chain DualKey](https://docs.m5stack.com/en/chain/Chain_DualKey). The left key denies a prompt, the right key approves it once, and the LEDs signal pairing and prompt attention.
- `esp_box_3_demo/`: a fuller [ESP-BOX-3](https://www.espressif.com/en/dev-board/esp32-s3-box-3-en) demo with on-device LVGL-based UI and `char_*` folder-push support for character packs.
- `assets/`: sample assets used by the example character-pack flow.

## Shared Example Components

`common/components/` contains small helpers used by these examples. They are example support code, not part of the public SDK surface.

- `example_app_helpers`: common NVS setup, persisted strings, Buddy snapshot caching, prompt replies, time sync, and BLE transport state updates.
- `example_charpack`: character-pack install and activation helpers, plus a folder-push sink for `char_*` transfers.
- `example_console`: a small REPL with shared `state` and `reply` commands.
- `example_status`: helpers for building compact `status.data` JSON replies.
- `example_store_nvs`: simple string read and write helpers for NVS.

## Where To Start

- Start with `generic_headless/` for the smallest integration example.
- Use `m5_dualkey_headless/` when you want a simple hardware prompt UI.
- Use `esp_box_3_demo/` when you need display UI or character-pack transfer.
