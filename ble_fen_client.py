#!/usr/bin/env python3
"""
SmartChess BLE FEN client.

Reads and subscribes to the FEN characteristic exposed by firmware:
  - Device name: SmartChess-ESP32S3
  - Service UUID: 3f0e0001-70a1-4f8a-a6a3-51e9590e9f20
  - Char UUID:    3f0e0002-70a1-4f8a-a6a3-51e9590e9f20

Dependency:
  pip install bleak
"""

from __future__ import annotations

import argparse
import asyncio
import sys
from datetime import datetime
from importlib import metadata as importlib_metadata

try:
    import bleak
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("Missing dependency: bleak")
    print("Install with: pip install bleak")
    sys.exit(1)


DEFAULT_DEVICE_NAME = "SmartChess-ESP32S3"
DEFAULT_SERVICE_UUID = "3f0e0001-70a1-4f8a-a6a3-51e9590e9f20"
DEFAULT_CHAR_UUID = "3f0e0002-70a1-4f8a-a6a3-51e9590e9f20"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Read SmartChess FEN over BLE")
    parser.add_argument("--name", default=DEFAULT_DEVICE_NAME, help="BLE device name")
    parser.add_argument("--address", default=None, help="BLE MAC/address (skip scan)")
    parser.add_argument("--service-uuid", default=DEFAULT_SERVICE_UUID, help="Service UUID")
    parser.add_argument("--char-uuid", default=DEFAULT_CHAR_UUID, help="Characteristic UUID")
    parser.add_argument("--scan-timeout", type=float, default=8.0, help="Scan timeout in seconds")
    parser.add_argument("--connect-timeout", type=float, default=10.0, help="Connect timeout in seconds")
    parser.add_argument("--duration", type=float, default=0.0, help="Listen duration in seconds (0 = forever)")
    parser.add_argument("--read-once", action="store_true", help="Read current FEN once and exit")
    parser.add_argument("--list", action="store_true", help="List nearby BLE devices and exit")
    parser.add_argument(
        "--print-duplicates",
        action="store_true",
        help="Print duplicated keepalive FEN notifications too",
    )
    return parser.parse_args()


def bleak_version() -> str:
    try:
        return importlib_metadata.version("bleak")
    except Exception:
        return "unknown"


def decode_payload(data: bytearray) -> str:
    try:
        return bytes(data).decode("utf-8").strip()
    except UnicodeDecodeError:
        return bytes(data).hex().upper()


def print_fen_details(fen: str) -> None:
    parts = fen.split()
    if len(parts) != 6:
        return

    placement, side, castling, en_passant, halfmove, fullmove = parts
    print(f"  placement : {placement}")
    print(f"  side      : {side}")
    print(f"  castling  : {castling}")
    print(f"  en-passant: {en_passant}")
    print(f"  halfmove  : {halfmove}")
    print(f"  fullmove  : {fullmove}")


async def list_devices(timeout: float) -> None:
    print(f"Scanning BLE devices for {timeout:.1f}s ...")
    devices = await BleakScanner.discover(timeout=timeout)
    if not devices:
        print("No BLE devices found.")
        return

    for dev in devices:
        name = dev.name or "<no-name>"
        print(f"- {name:30} {dev.address}")


async def find_device_address(name: str, timeout: float) -> str:
    devices = await BleakScanner.discover(timeout=timeout)
    if not devices:
        raise RuntimeError("No BLE devices discovered")

    exact = [d for d in devices if (d.name or "") == name]
    if exact:
        return exact[0].address

    contains = [d for d in devices if name.lower() in (d.name or "").lower()]
    if contains:
        return contains[0].address

    raise RuntimeError(f"Device not found by name: {name}")


async def run_client(args: argparse.Namespace) -> None:
    if args.list:
        await list_devices(args.scan_timeout)
        return

    address = args.address
    if not address:
        print(f"Scanning for device name '{args.name}' ...")
        address = await find_device_address(args.name, args.scan_timeout)

    print(f"Connecting to: {address}")
    async with BleakClient(address, timeout=args.connect_timeout) as client:
        print("Connected")
        print(f"Bleak version: {bleak_version()}")

        services = None
        if hasattr(client, "get_services"):
            # Bleak <= 0.x style
            services = await client.get_services()
        elif hasattr(client, "services"):
            # Bleak >= 1.x style
            services = client.services

        if services is not None and hasattr(services, "get_service"):
            service = services.get_service(args.service_uuid)
            if service is None:
                print(f"Warning: service {args.service_uuid} not found (continuing)")
        else:
            print("Info: skip explicit service check (Bleak API variant)")

        current_raw = await client.read_gatt_char(args.char_uuid)
        current_fen = decode_payload(current_raw)
        print(f"Current FEN: {current_fen}")
        print_fen_details(current_fen)

        last_seen = {"fen": current_fen}

        if args.read_once:
            return

        def on_notify(_: int, data: bytearray) -> None:
            fen = decode_payload(data)
            if not args.print_duplicates and fen == last_seen["fen"]:
                return
            last_seen["fen"] = fen
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] FEN: {fen}")

        await client.start_notify(args.char_uuid, on_notify)
        print("Subscribed to FEN notify")
        print("Press Ctrl+C to stop")

        try:
            if args.duration > 0:
                await asyncio.sleep(args.duration)
            else:
                while True:
                    await asyncio.sleep(1.0)
        finally:
            if client.is_connected:
                await client.stop_notify(args.char_uuid)


def main() -> None:
    args = parse_args()
    try:
        asyncio.run(run_client(args))
    except KeyboardInterrupt:
        print("Stopped by user")
    except Exception as exc:
        print(f"Error: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
