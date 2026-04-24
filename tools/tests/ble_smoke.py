#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""BLE smoke tests for the Desktop Buddy examples."""

from __future__ import annotations

import argparse
import asyncio
from pathlib import Path

from ble_harness import (
    BumbleBuddySession,
    require,
    scan_esp_desktop_buddy_targets,
    upload_character_pack,
)

DEFAULT_CHARACTER_DIR = (
    Path(__file__).resolve().parent / "fixtures" / "box-smoke-pack"
)
AUTO_ONCE_HINT = "__auto_once__"
AUTO_DENY_HINT = "__auto_deny__"


def status_data(status_ack: dict) -> dict:
    return status_ack.get("data", {})


def buddy_has_state(buddy_status: dict) -> bool | None:
    if "haveState" in buddy_status:
        return buddy_status.get("haveState")
    return buddy_status.get("haveSnapshot")


def session_kwargs_from_args(args: argparse.Namespace) -> dict:
    return dict(
        transport=args.transport,
        serial_port=args.serial_port,
        serial_baud=args.serial_baud,
        boot_wait=args.boot_wait,
        name_prefix=args.name_prefix,
        exact_name=args.name,
        scan_timeout=args.scan_timeout,
        request_mtu=args.request_mtu,
        local_address=args.local_address,
    )


async def run_scan(args: argparse.Namespace) -> int:
    results = await scan_esp_desktop_buddy_targets(
        transport=args.transport,
        name_prefix=args.name_prefix,
        timeout_s=args.scan_timeout,
        local_address=args.local_address,
    )
    if not results:
        print("No Desktop Buddy devices found")
        return 1

    for result in results:
        print(f"{result.name}\t{result.address}\tRSSI {result.rssi}")
    return 0


async def run_generic_headless(args: argparse.Namespace) -> int:
    session_kwargs = session_kwargs_from_args(args)
    avoid_names: set[str] = set()
    reconnect_name = args.name

    for _ in range(4):
        attempt_kwargs = dict(session_kwargs)
        if avoid_names:
            attempt_kwargs["avoid_names"] = tuple(sorted(avoid_names))

        async with BumbleBuddySession(**attempt_kwargs) as session:
            reconnect_name = session.target_name
            require(session.target_name.startswith(args.name_prefix), "unexpected advertising name")

            preflight_status = await session.request_status()
            preflight_data = status_data(preflight_status)
            if "buddy" not in preflight_data or "pack" in preflight_data:
                if args.name is not None:
                    raise RuntimeError(
                        "selected target does not expose generic headless status payload: "
                        f"{preflight_data}"
                    )
                avoid_names.add(session.target_name)
                continue

            name_ack = await session.send_command_ack(
                {"cmd": "name", "name": args.display_name}, "name"
            )
            require(name_ack.get("ok") is True, f"name command failed: {name_ack}")

            owner_ack = await session.send_command_ack(
                {"cmd": "owner", "name": args.owner_name}, "owner"
            )
            require(owner_ack.get("ok") is True, f"owner command failed: {owner_ack}")

            status_ack = await session.request_status()
            data = status_data(status_ack)
            buddy = data.get("buddy", {})
            require(data.get("name") == args.display_name, f"unexpected status name: {data}")
            require(data.get("owner") == args.owner_name, f"unexpected status owner: {data}")
            require(data.get("sec") is True, f"status did not report secure link: {data}")
            require(
                buddy_has_state(buddy) is False, f"unexpected initial snapshot state: {buddy}"
            )

            await session.send_json(
                {
                    "total": 2,
                    "running": 1,
                    "waiting": 1,
                    "msg": "first",
                    "tokens": 11,
                    "tokens_today": 22,
                    "entries": ["alpha", "beta"],
                    "prompt": {"id": "p-first", "tool": "Bash", "hint": "manual"},
                }
            )
            status_ack = await session.request_status()
            buddy = status_data(status_ack).get("buddy", {})
            require(buddy.get("running") == 1, f"first snapshot not reflected: {buddy}")
            require(buddy.get("waiting") == 1, f"first snapshot not reflected: {buddy}")
            require(buddy.get("entries") == ["alpha", "beta"], f"entries not reflected: {buddy}")
            require(
                buddy.get("prompt", {}).get("present") is True
                and buddy.get("prompt", {}).get("id") == "p-first",
                f"prompt not reflected: {buddy}",
            )

            await session.send_json(
                {
                    "total": 4,
                    "running": 3,
                    "waiting": 1,
                    "msg": "second",
                    "tokens": 33,
                    "tokens_today": 44,
                }
            )
            status_ack = await session.request_status()
            buddy = status_data(status_ack).get("buddy", {})
            require(buddy.get("msg") == "second", f"replacement snapshot msg mismatch: {buddy}")
            require(
                buddy.get("entries") == [], f"replacement snapshot retained entries: {buddy}"
            )
            require(
                buddy.get("prompt", {}).get("present") is False,
                f"replacement snapshot retained prompt: {buddy}",
            )

            await session.send_json(
                {
                    "total": 6,
                    "running": 1,
                    "waiting": 5,
                    "msg": "bad prompt",
                    "tokens": 77,
                    "tokens_today": 88,
                    "prompt": {"id": 5, "tool": "Bash", "hint": "bad"},
                }
            )
            status_ack = await session.request_status()
            buddy = status_data(status_ack).get("buddy", {})
            require(
                buddy.get("prompt", {}).get("present") is False,
                f"malformed prompt did not degrade to absent: {buddy}",
            )

            before_invalid = buddy
            await session.send_json(
                {
                    "total": "oops",
                    "running": 9,
                    "waiting": 0,
                    "msg": "invalid",
                    "tokens": 99,
                    "tokens_today": 111,
                }
            )
            status_ack = await session.request_status()
            after_invalid = status_data(status_ack).get("buddy", {})
            require(
                after_invalid.get("msg") == before_invalid.get("msg")
                and after_invalid.get("total") == before_invalid.get("total"),
                f"malformed required snapshot fields should not mutate state: {after_invalid}",
            )

            await session.send_json(
                {
                    "total": 7,
                    "running": 1,
                    "waiting": 1,
                    "msg": "auto once",
                    "tokens": 10,
                    "tokens_today": 12,
                    "prompt": {"id": "p-auto-once", "tool": "Bash", "hint": AUTO_ONCE_HINT},
                }
            )
            reply = await session.wait_for_payload(
                lambda payload: payload.get("cmd") == "permission"
                and payload.get("id") == "p-auto-once",
                timeout_s=args.reply_timeout,
            )
            require(
                reply.get("decision") == "once",
                f"unexpected auto-once permission reply: {reply}",
            )
            status_ack = await session.request_status()
            require(
                status_data(status_ack).get("stats", {}).get("appr", 0) >= 1,
                f"approval counter did not increment: {status_ack}",
            )

            await session.send_json(
                {
                    "total": 8,
                    "running": 1,
                    "waiting": 1,
                    "msg": "auto deny",
                    "tokens": 13,
                    "tokens_today": 15,
                    "prompt": {"id": "p-auto-deny", "tool": "Edit", "hint": AUTO_DENY_HINT},
                }
            )
            reply = await session.wait_for_payload(
                lambda payload: payload.get("cmd") == "permission"
                and payload.get("id") == "p-auto-deny",
                timeout_s=args.reply_timeout,
            )
            require(
                reply.get("decision") == "deny",
                f"unexpected auto-deny permission reply: {reply}",
            )
            status_ack = await session.request_status()
            require(
                status_data(status_ack).get("stats", {}).get("deny", 0) >= 1,
                f"denial counter did not increment: {status_ack}",
            )

            await session.send_json({"time": [1713571200, 19800]})
            status_ack = await session.request_status()
            buddy = status_data(status_ack).get("buddy", {})
            require(buddy.get("tz") == 19800, f"time sync was not applied: {buddy}")

            await session.send_json(
                {"evt": "turn", "role": "assistant", "content": {"text": "hi"}}
            )
            status_ack = await session.request_status()
            data = status_data(status_ack)
            buddy = data.get("buddy", {})
            require(
                data.get("stats", {}).get("turns", 0) >= 1,
                f"turn counter did not increment: {data}",
            )
            require(
                buddy.get("turn", {}).get("role") == "assistant"
                and buddy.get("turn", {}).get("len", 0) > 0,
                f"turn payload was not reflected: {buddy}",
            )
            break
    else:
        raise RuntimeError("no generic headless target matched the expected status payload")

    reconnect_kwargs = dict(session_kwargs)
    reconnect_kwargs["exact_name"] = reconnect_name
    async with BumbleBuddySession(**reconnect_kwargs) as session:
        status_ack = await session.request_status()
        data = status_data(status_ack)
        require(data.get("name") == args.display_name, f"display name did not persist: {data}")
        require(data.get("owner") == "", f"owner did not clear on reconnect: {data}")

    print("Generic headless BLE smoke passed")
    return 0


async def run_bonded_reconnect(args: argparse.Namespace) -> int:
    session_kwargs = session_kwargs_from_args(args)

    async with BumbleBuddySession(**session_kwargs) as session:
        status_ack = await session.request_status()
        data = status_data(status_ack)
        require(data.get("sec") is True, f"initial session did not report secure link: {data}")
        reconnect_name = session.target_name

    reconnect_kwargs = dict(session_kwargs)
    reconnect_kwargs["exact_name"] = reconnect_name
    async with BumbleBuddySession(**reconnect_kwargs) as session:
        status_ack = await session.request_status()
        data = status_data(status_ack)
        require(data.get("sec") is True, f"reconnect did not report secure link: {data}")
        require(
            session.target_name == reconnect_name,
            f"reconnect matched unexpected target {session.target_name} (wanted {reconnect_name})",
        )

    print("Bonded reconnect BLE smoke passed")
    return 0


async def run_box_charpack(args: argparse.Namespace) -> int:
    async with BumbleBuddySession(
        **session_kwargs_from_args(args),
    ) as session:
        status_ack = await session.request_status()
        require(
            "pack" in status_data(status_ack),
            f"box demo status payload does not expose pack info: {status_ack}",
        )

        pack_name = await upload_character_pack(
            session, Path(args.character_dir).resolve(), args.raw_chunk_size
        )
        status_ack = await session.request_status(timeout_s=15.0)
        pack = status_data(status_ack).get("pack", {})
        require(
            pack.get("status") == f"Installed {pack_name}"
            or pack.get("status") == f"Active {pack_name}",
            f"unexpected pack status after upload: {pack}",
        )
        require(
            pack.get("active") == pack_name,
            f"unexpected active pack after upload: {pack}",
        )

        print("ESP-BOX-3 charpack BLE smoke passed")
        return 0


def add_transport_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--transport", default="usb:0", help="HCI transport for the local test controller")
    parser.add_argument("--name-prefix", default="Claude-", help="Advertising name prefix")
    parser.add_argument("--scan-timeout", type=float, default=10.0, help="Scan timeout")
    parser.add_argument("--local-address", help="Override the local test controller address")


def add_session_args(parser: argparse.ArgumentParser) -> None:
    add_transport_args(parser)
    parser.add_argument("--name", required=True, help="Exact advertising name to match")
    parser.add_argument("--request-mtu", type=int, default=185, help="Requested ATT MTU")
    parser.add_argument("--serial-port", required=True, help="Console port for passkey output")
    parser.add_argument("--serial-baud", type=int, default=115200, help="Console baud")
    parser.add_argument("--boot-wait", type=float, default=1.0, help="Boot settle time")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="BLE smoke tests for Desktop Buddy examples"
    )
    subparsers = parser.add_subparsers(dest="suite", required=True)

    scan = subparsers.add_parser("scan", help="List nearby Desktop Buddy BLE targets")
    add_transport_args(scan)

    generic = subparsers.add_parser("generic-headless", help="Test the generic headless example")
    add_session_args(generic)
    generic.add_argument(
        "--reply-timeout",
        type=float,
        default=5.0,
        help="Seconds to wait for prompt reply notifications",
    )
    generic.add_argument(
        "--display-name",
        default="Buddy Smoke",
        help="Display name to set with the public name command",
    )
    generic.add_argument(
        "--owner-name",
        default="Tester",
        help="Owner name to set with the public owner command",
    )

    reconnect = subparsers.add_parser(
        "bonded-reconnect",
        help="Test a bonded reconnect against any status-capable Buddy example",
    )
    add_session_args(reconnect)

    charpack = subparsers.add_parser("box-charpack", help="Test ESP-BOX-3 folder push")
    add_session_args(charpack)
    charpack.add_argument(
        "--character-dir",
        default=str(DEFAULT_CHARACTER_DIR),
        help="Flat character-pack directory to upload (defaults to the minimal smoke fixture)",
    )
    charpack.add_argument(
        "--raw-chunk-size",
        type=int,
        default=120,
        help="Preferred bytes per raw file chunk before base64; auto-clamped to the ATT MTU",
    )

    return parser.parse_args()


async def run(args: argparse.Namespace) -> int:
    if args.suite == "scan":
        return await run_scan(args)
    if args.suite == "generic-headless":
        return await run_generic_headless(args)
    if args.suite == "bonded-reconnect":
        return await run_bonded_reconnect(args)
    if args.suite == "box-charpack":
        return await run_box_charpack(args)
    raise RuntimeError(f"unknown suite {args.suite}")


def main() -> int:
    return asyncio.run(run(parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
