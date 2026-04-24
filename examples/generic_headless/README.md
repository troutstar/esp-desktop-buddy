# Generic Headless

Smallest Desktop Buddy example. No board UI, no character-pack support, just BLE + serial console.

## Hardware

- Any BLE-capable ESP32 with a working serial console

## Build

```bash
idf.py set-target <esp32-target>
idf.py flash monitor
```

On boot, the device advertises as `Claude-XXXX`.

## Use

- Pair from `Developer -> Open Hardware Buddy...` in Claude Desktop.
- Enter the passkey shown on the serial console if pairing asks for it.
- The onboard LED blinks during pairing and while a prompt is waiting.

## Console

```text
status
reply once
reply deny
unpair
```
