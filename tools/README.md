# Tools

Host-side tools for this repository.

- `tests/ble_smoke.py`: BLE smoke tests
- `tests/ble_harness.py`: shared helpers for the smoke tests
- `tests/fixtures/`: fixtures used by the smoke tests

## BLE Smoke Setup

Install the Python dependencies before running the smoke tests:

```bash
python -m pip install bumble pyserial
```

The smoke tests pass `--transport` directly to Bumble. Common values are `usb:0` for the first USB HCI adapter and `serial:/dev/ttyUSB0` for a UART HCI adapter.

See [`docs/testing.md`](../docs/testing.md).
