# ESP-BOX-3 Demo

Full Desktop Buddy demo for ESP-BOX-3 with on-device UI and character-pack transfer support.

<a href="../../docs/static/esp-box-3-demo.png">
  <img src="../../docs/static/esp-box-3-demo.png" alt="ESP-BOX-3 Desktop Buddy demo showing on-device prompt approval UI" width="460">
</a>

## Hardware

- ESP32-S3-BOX-3

## Build

```bash
idf.py set-target esp32s3
idf.py flash monitor
```

On boot, the device advertises as `Claude-XXXX`.

## Use

- Pair from `Developer -> Open Hardware Buddy...` in Claude Desktop.
- The display shows the pairing passkey when needed.
- `MAIN` approves the current prompt once.
- `BOOT` denies the current prompt.
- Uploaded character packs become active automatically.

## Console

```text
status
reply once
reply deny
packs
pack use <index>
unpair
```
