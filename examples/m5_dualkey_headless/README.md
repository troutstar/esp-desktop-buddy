# M5 DualKey Headless

Desktop Buddy example for an M5Stack Chain DualKey on an ESP32-S3 host. Uses hardware keys and LEDs for prompt handling.

## Hardware

- ESP32-S3 host board
- M5Stack Chain DualKey

## Build

```bash
idf.py set-target esp32s3
idf.py flash monitor
```

On boot, the device advertises as `Claude-XXXX`.

## Use

- Pair from `Developer -> Open Hardware Buddy...` in Claude Desktop.
- Enter the serial passkey if pairing asks for it.
- Left key denies the current prompt.
- Right key approves the current prompt once.

## Console

```text
status
reply once
reply deny
unpair
```
