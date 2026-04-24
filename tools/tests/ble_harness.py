#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Shared helpers for the BLE smoke tests."""

from __future__ import annotations

import asyncio
import base64
import json
import re
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import serial
from bumble.core import AdvertisingData
from bumble.device import Advertisement, Device, Peer
from bumble.keys import JsonKeyStore
from bumble.pairing import PairingConfig, PairingDelegate
from bumble.transport import open_transport

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEFAULT_LOCAL_ADDRESS = "F0:F1:F2:00:00:01"
DEFAULT_KEYSTORE_DIR = Path(__file__).resolve().parent / ".artifacts" / "bumble-keystore"


@dataclass
class NotificationStream:
    buffer: bytearray
    queue: asyncio.Queue[dict[str, Any]]

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.queue = asyncio.Queue()

    def feed(self, data: bytes) -> None:
        self.buffer.extend(data)
        while True:
            newline_index = self.buffer.find(b"\n")
            if newline_index < 0:
                return
            raw = bytes(self.buffer[:newline_index]).rstrip(b"\r")
            del self.buffer[: newline_index + 1]
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace")
            self.queue.put_nowait(json.loads(text))

    async def next(self, timeout_s: float = 10.0) -> dict[str, Any]:
        return await asyncio.wait_for(self.queue.get(), timeout=timeout_s)

    async def wait_for(
        self,
        predicate: Callable[[dict[str, Any]], bool],
        timeout_s: float = 10.0,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for matching notification")
            payload = await self.next(timeout_s=remaining)
            if predicate(payload):
                return payload

    def clear(self) -> None:
        while not self.queue.empty():
            self.queue.get_nowait()


@dataclass(frozen=True)
class BuddyScanResult:
    name: str
    address: str
    rssi: int


def safe_path_token(text: str) -> str:
    token = re.sub(r"[^A-Za-z0-9._-]+", "-", text).strip("-")
    return token or "device"


def keystore_path_for_target(target_name: str) -> Path:
    return DEFAULT_KEYSTORE_DIR / f"{safe_path_token(target_name)}.json"


class ControlSerial:
    def __init__(self, port: str, baud: int, boot_wait: float) -> None:
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.2, dsrdtr=False, rtscts=False)
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except OSError:
            pass
        time.sleep(boot_wait)
        self.ser.reset_input_buffer()
        self._lock = threading.Lock()
        self._line_buffer = ""
        self._output = ""
        self._passkeys: list[int] = []
        self._next_passkey_index = 0
        self._stop = False
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def _reader_loop(self) -> None:
        while not self._stop:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                break
            if not chunk:
                continue
            text = chunk.decode("utf-8", errors="replace")
            with self._lock:
                self._output += text
                self._line_buffer += text
                while "\n" in self._line_buffer:
                    line, self._line_buffer = self._line_buffer.split("\n", 1)
                    line = line.rstrip("\r")
                    match = re.search(r"pairing passkey (\d{6})", line)
                    if match:
                        self._passkeys.append(int(match.group(1)))

    def drain_output(self) -> str:
        with self._lock:
            text = self._output
            self._output = ""
        return text

    async def wait_for_passkey(self, timeout_s: float) -> int:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            with self._lock:
                if self._next_passkey_index < len(self._passkeys):
                    passkey = self._passkeys[self._next_passkey_index]
                    self._next_passkey_index += 1
                    return passkey
            await asyncio.sleep(0.1)
        raise TimeoutError("pairing passkey was not observed on serial output")

    def close(self) -> None:
        self._stop = True
        self._thread.join(timeout=1.0)
        self.ser.close()


class SerialPasskeyDelegate(PairingDelegate):
    def __init__(self, control_serial: ControlSerial) -> None:
        super().__init__(io_capability=PairingDelegate.KEYBOARD_INPUT_ONLY)
        self.control_serial = control_serial

    async def accept(self) -> bool:
        return True

    async def get_number(self) -> int | None:
        return await self.control_serial.wait_for_passkey(timeout_s=20.0)

    async def confirm(self, auto: bool = False) -> bool:
        return True


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def advertisement_name(advertisement: Advertisement) -> str | None:
    return advertisement.data.get(
        AdvertisingData.Type.COMPLETE_LOCAL_NAME
    ) or advertisement.data.get(AdvertisingData.Type.SHORTENED_LOCAL_NAME)


def advertisement_matches_name(
    advertisement: Advertisement,
    name_prefix: str,
    exact_name: str | None,
    avoid_names: set[str] | None = None,
) -> bool:
    name = advertisement_name(advertisement)
    if not name:
        return False
    if avoid_names and name in avoid_names:
        return False
    if exact_name is not None:
        return name == exact_name
    return name.startswith(name_prefix)


def advertisement_has_nus(advertisement: Advertisement) -> bool:
    ad_types = (
        AdvertisingData.Type.COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
        AdvertisingData.Type.INCOMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    )
    for ad_type in ad_types:
        uuids = advertisement.data.get(ad_type) or []
        if any(str(uuid).lower() == NUS_SERVICE_UUID for uuid in uuids):
            return True
    return False


async def wait_for_target(
    device: Device,
    name_prefix: str,
    exact_name: str | None,
    timeout_s: float,
    avoid_names: set[str] | None = None,
) -> Advertisement:
    loop = asyncio.get_running_loop()
    future: asyncio.Future[Advertisement] = loop.create_future()

    def on_advertisement(advertisement: Advertisement) -> None:
        if not advertisement_matches_name(
            advertisement, name_prefix, exact_name, avoid_names=avoid_names
        ):
            return
        if not advertisement_has_nus(advertisement):
            return
        if not future.done():
            future.set_result(advertisement)

    device.on("advertisement", on_advertisement)
    await device.start_scanning(active=True, filter_duplicates=False)
    try:
        return await asyncio.wait_for(future, timeout=timeout_s)
    finally:
        await device.stop_scanning()


async def scan_esp_desktop_buddy_targets(
    transport: str,
    name_prefix: str,
    timeout_s: float,
    local_address: str | None,
) -> list[BuddyScanResult]:
    results: dict[str, BuddyScanResult] = {}
    transport_ctx = await open_transport(transport)

    async with transport_ctx as (hci_source, hci_sink):
        device = Device.with_hci(
            "BumbleScan", local_address or make_local_address(), hci_source, hci_sink
        )
        await device.power_on()

        def on_advertisement(advertisement: Advertisement) -> None:
            if not advertisement_matches_name(advertisement, name_prefix, exact_name=None):
                return
            if not advertisement_has_nus(advertisement):
                return

            name = advertisement_name(advertisement)
            if not name:
                return

            address = str(advertisement.address)
            current = results.get(address)
            if current is None or advertisement.rssi > current.rssi:
                results[address] = BuddyScanResult(
                    name=name,
                    address=address,
                    rssi=advertisement.rssi,
                )

        device.on("advertisement", on_advertisement)
        await device.start_scanning(active=True, filter_duplicates=False)
        try:
            await asyncio.sleep(timeout_s)
        finally:
            await device.stop_scanning()
            await device.power_off()

    return sorted(results.values(), key=lambda item: item.rssi, reverse=True)


async def find_nus_characteristics(peer: Peer):
    services = await peer.discover_service(NUS_SERVICE_UUID)
    require(bool(services), "Desktop Buddy BLE service not found on peer")

    service = services[0]
    characteristics = await peer.discover_characteristics(service=service)
    rx_char = None
    tx_char = None

    for characteristic in characteristics:
        uuid = str(characteristic.uuid).lower()
        if uuid == NUS_RX_UUID:
            rx_char = characteristic
        elif uuid == NUS_TX_UUID:
            tx_char = characteristic

    require(rx_char is not None, "Desktop Buddy RX characteristic was not discovered")
    require(tx_char is not None, "Desktop Buddy TX characteristic was not discovered")
    return rx_char, tx_char


def make_local_address() -> str:
    return DEFAULT_LOCAL_ADDRESS


class BumbleBuddySession:
    def __init__(
        self,
        transport: str,
        serial_port: str,
        serial_baud: int,
        boot_wait: float,
        name_prefix: str,
        exact_name: str | None,
        scan_timeout: float,
        request_mtu: int,
        local_address: str | None,
        avoid_names: tuple[str, ...] | None = None,
    ) -> None:
        self.transport_name = transport
        self.control_serial = ControlSerial(serial_port, serial_baud, boot_wait)
        self.name_prefix = name_prefix
        self.exact_name = exact_name
        self.scan_timeout = scan_timeout
        self.request_mtu = request_mtu
        self.local_address = local_address or make_local_address()
        self.avoid_names = set(avoid_names or ())
        self._transport_ctx = None
        self._transport = None
        self.device: Device | None = None
        self.connection = None
        self.peer: Peer | None = None
        self.rx_char = None
        self.tx_char = None
        self.notifications = NotificationStream()
        self.target_name = "<unknown>"
        self.keystore_path: Path | None = None
        self.att_mtu = 23

    async def __aenter__(self) -> "BumbleBuddySession":
        boot_output = self.control_serial.drain_output()
        if boot_output:
            print("=== Serial boot output ===")
            print(boot_output.rstrip())

        self._transport_ctx = await open_transport(self.transport_name)
        self._transport = await self._transport_ctx.__aenter__()
        hci_source, hci_sink = self._transport
        self.device = Device.with_hci("Bumble", self.local_address, hci_source, hci_sink)
        keystore_name = self.exact_name or self.name_prefix
        self.keystore_path = keystore_path_for_target(keystore_name)
        self.keystore_path.parent.mkdir(parents=True, exist_ok=True)
        self.device.keystore = JsonKeyStore.from_device(
            self.device, filename=str(self.keystore_path)
        )
        self.device.pairing_config_factory = lambda _connection: PairingConfig(
            sc=True,
            mitm=True,
            bonding=True,
            delegate=SerialPasskeyDelegate(self.control_serial),
        )
        await self.device.power_on()
        await self.connect()
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        if self.connection is not None:
            try:
                await self.connection.disconnect()
            except Exception:
                pass
        if self.device is not None:
            try:
                await self.device.power_off()
            except Exception:
                pass
        if self._transport_ctx is not None:
            await self._transport_ctx.__aexit__(exc_type, exc, tb)
        self.control_serial.close()

    async def connect(self) -> None:
        require(self.device is not None, "device is not initialized")
        print(f"Using local Bumble address {self.local_address}")
        advertisement = await wait_for_target(
            self.device,
            self.name_prefix,
            self.exact_name,
            self.scan_timeout,
            avoid_names=self.avoid_names,
        )
        self.target_name = advertisement_name(advertisement) or "<unknown>"
        print(f"Found {self.target_name} at {advertisement.address} (RSSI {advertisement.rssi})")

        self.connection = await self.device.connect(advertisement.address)
        pairing_done: asyncio.Future[None] = asyncio.get_running_loop().create_future()
        pairing_task: asyncio.Task | None = None
        stored_keys = None

        if self.device.keystore is not None:
            stored_keys = await self.device.keystore.get(str(self.connection.peer_address))

        async def drive_security() -> None:
            if stored_keys is None:
                await self.connection.pair()
            else:
                print(f"Reusing stored bond for {self.connection.peer_address}")
                try:
                    await asyncio.wait_for(self.connection.encrypt(), timeout=5.0)
                    if not self.connection.is_encrypted:
                        raise RuntimeError("stored bond did not produce an encrypted link")
                except Exception:
                    if self.device.keystore is not None:
                        await self.device.keystore.delete(str(self.connection.peer_address))
                    print(f"Stored bond for {self.connection.peer_address} was stale; pairing again")
                    await self.connection.pair()
            if self.connection.is_encrypted and not pairing_done.done():
                pairing_done.set_result(None)

        pairing_task = asyncio.create_task(drive_security())

        def on_encryption_change() -> None:
            if self.connection.is_encrypted and not pairing_done.done():
                pairing_done.set_result(None)

        def on_pairing_failure(reason) -> None:
            if not pairing_done.done():
                pairing_done.set_exception(RuntimeError(f"pairing failure: {reason}"))

        def on_security_task_done(task: asyncio.Task) -> None:
            if task.cancelled() or pairing_done.done():
                return
            error = task.exception()
            if error is not None:
                pairing_done.set_exception(error)

        self.connection.on(self.connection.EVENT_CONNECTION_ENCRYPTION_CHANGE, on_encryption_change)
        self.connection.on(self.connection.EVENT_PAIRING_FAILURE, on_pairing_failure)
        pairing_task.add_done_callback(on_security_task_done)
        if self.connection.is_encrypted and not pairing_done.done():
            pairing_done.set_result(None)
        try:
            await asyncio.wait_for(pairing_done, timeout=20.0)
        except asyncio.TimeoutError as exc:
            serial_output = self.control_serial.drain_output().strip()
            if serial_output:
                print("=== Serial output during pairing timeout ===")
                print(serial_output)
            if pairing_task is not None:
                pairing_task.cancel()
            raise asyncio.TimeoutError("timed out waiting for link encryption") from exc

        self.peer = Peer(self.connection)
        mtu = await self.peer.request_mtu(self.request_mtu)
        self.att_mtu = mtu
        print(f"Negotiated ATT MTU: {mtu}")

        self.rx_char, self.tx_char = await find_nus_characteristics(self.peer)
        self.notifications = NotificationStream()
        await self.peer.subscribe(self.tx_char, self.notifications.feed)
        print(f"Subscribed to {self.target_name} notifications")

    async def reconnect(self) -> None:
        require(self.connection is not None, "no active connection")
        await self.connection.disconnect()
        self.connection = None
        self.peer = None
        self.rx_char = None
        self.tx_char = None
        self.notifications.clear()
        await asyncio.sleep(3.0)
        await self.connect()

    async def send_json(self, payload: dict[str, Any]) -> None:
        require(self.peer is not None and self.rx_char is not None, "session is not connected")
        line = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
        request_limit = max(20, self.att_mtu - 3)
        with_response = len(line) > request_limit
        print(f"TX {line.decode('utf-8').rstrip()}")
        await self.peer.write_value(self.rx_char, line, with_response=with_response)

    async def wait_for_payload(
        self,
        predicate: Callable[[dict[str, Any]], bool],
        timeout_s: float = 10.0,
    ) -> dict[str, Any]:
        try:
            payload = await self.notifications.wait_for(predicate, timeout_s=timeout_s)
        except TimeoutError as exc:
            serial_output = self.control_serial.drain_output().strip()
            if serial_output:
                print("=== Serial output during notification timeout ===")
                print(serial_output)
            raise TimeoutError("timed out waiting for matching Buddy notification") from exc
        print(f"RX {json.dumps(payload, separators=(',', ':'))}")
        return payload

    async def send_command_ack(
        self,
        payload: dict[str, Any],
        ack: str,
        timeout_s: float = 10.0,
    ) -> dict[str, Any]:
        await self.send_json(payload)
        return await self.wait_for_payload(lambda item: item.get("ack") == ack, timeout_s)

    async def request_status(self, timeout_s: float = 10.0) -> dict[str, Any]:
        return await self.send_command_ack({"cmd": "status"}, "status", timeout_s=timeout_s)


def iter_character_files(character_dir: Path) -> list[Path]:
    files = [
        path
        for path in character_dir.iterdir()
        if path.is_file() and not path.name.startswith(".")
    ]
    return sorted(files, key=lambda path: (path.name != "manifest.json", path.name))


def resolve_pack_name(character_dir: Path) -> str:
    manifest_path = character_dir / "manifest.json"
    if not manifest_path.is_file():
        return character_dir.name

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return character_dir.name

    manifest_name = manifest.get("name")
    if isinstance(manifest_name, str) and manifest_name:
        return manifest_name
    return character_dir.name


def clamp_raw_chunk_size_for_mtu(raw_chunk_size: int, att_mtu: int) -> int:
    command_overhead = len(json.dumps({"cmd": "chunk", "d": ""}, separators=(",", ":"))) + 1
    att_payload_size = max(20, att_mtu - 3)
    safe_raw_chunk_size = raw_chunk_size

    while safe_raw_chunk_size > 0:
        encoded_len = 4 * ((safe_raw_chunk_size + 2) // 3)
        if command_overhead + encoded_len <= att_payload_size:
            return safe_raw_chunk_size
        safe_raw_chunk_size -= 1

    raise RuntimeError(
        f"ATT MTU {att_mtu} is too small for chunk command overhead ({command_overhead} bytes)"
    )


async def upload_character_pack(
    session: BumbleBuddySession,
    character_dir: Path,
    raw_chunk_size: int,
) -> str:
    files = iter_character_files(character_dir)
    require(bool(files), f"no files found in {character_dir}")

    pack_name = resolve_pack_name(character_dir)
    total_bytes = sum(path.stat().st_size for path in files)
    safe_raw_chunk_size = clamp_raw_chunk_size_for_mtu(raw_chunk_size, session.att_mtu)
    if safe_raw_chunk_size != raw_chunk_size:
        print(
            "Clamped raw chunk size from "
            f"{raw_chunk_size} to {safe_raw_chunk_size} for ATT MTU {session.att_mtu}"
        )
    begin_ack = await session.send_command_ack(
        {"cmd": "char_begin", "name": pack_name, "total": total_bytes},
        "char_begin",
        timeout_s=15.0,
    )
    require(begin_ack.get("ok") is True, f"char_begin failed: {begin_ack}")

    for path in files:
        file_ack = await session.send_command_ack(
            {"cmd": "file", "path": path.name, "size": path.stat().st_size},
            "file",
        )
        require(file_ack.get("ok") is True, f"file failed for {path.name}: {file_ack}")

        with path.open("rb") as handle:
            while True:
                chunk = handle.read(safe_raw_chunk_size)
                if not chunk:
                    break
                chunk_ack = await session.send_command_ack(
                    {"cmd": "chunk", "d": base64.b64encode(chunk).decode("ascii")},
                    "chunk",
                )
                require(chunk_ack.get("ok") is True, f"chunk failed for {path.name}: {chunk_ack}")

        end_ack = await session.send_command_ack({"cmd": "file_end"}, "file_end")
        require(end_ack.get("ok") is True, f"file_end failed for {path.name}: {end_ack}")

    char_end_ack = await session.send_command_ack(
        {"cmd": "char_end"}, "char_end", timeout_s=20.0
    )
    require(char_end_ack.get("ok") is True, f"char_end failed: {char_end_ack}")
    return pack_name
