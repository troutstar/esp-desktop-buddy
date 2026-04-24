# Testing

There are two types of tests available:

- component `test_apps` for non-BLE logic
- `tools/tests/ble_smoke.py` for end-to-end BLE validation

## Component Tests

Run the relevant `test_apps` under:

- `components/esp_desktop_buddy/test_apps/`
- `components/esp_desktop_buddy_folder_push/test_apps/`

Typical build flow:

```bash
cd components/esp_desktop_buddy/test_apps/core
idf.py set-target <ble-capable-esp32-target>
idf.py build
idf.py -p <serial-port> flash monitor
```

## BLE Smoke

```bash
python tools/tests/ble_smoke.py scan --transport <hci-transport>
python tools/tests/ble_smoke.py generic-headless \
  --transport <hci-transport> \
  --serial-port <serial-port> \
  --name Claude-1234
```

Folder-push smoke:

```bash
python tools/tests/ble_smoke.py box-charpack \
  --transport <hci-transport> \
  --serial-port <serial-port> \
  --name Claude-1234
```
