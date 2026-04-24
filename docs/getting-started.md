# Getting Started

Start with `examples/generic_headless`.

## Prerequisites

- ESP-IDF `v5.4+`
- a BLE-capable ESP32
- a serial console

## Build

```bash
cd examples/generic_headless
idf.py set-target <ble-capable-esp32-target>
idf.py -p <serial-port> flash monitor
```

On boot, the device advertises as `Claude-%02X%02X`, using the last two bytes of the Bluetooth MAC address.

## Verify

- In Claude for macOS or Windows, enable developer mode: `Help -> Troubleshooting -> Enable Developer Mode`.
- Open `Developer -> Open Hardware Buddy...`.
- Click `Connect` and select the device.
- Grant Bluetooth permission on first connect if the OS prompts for it.
- If prompted, enter the passkey shown on the serial console.
- Once paired, the desktop bridge should reconnect automatically.

## Next

- [Integration](integration.md)
