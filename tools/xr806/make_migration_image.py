#!/usr/bin/env python3
"""Build a *wired/serial first-install* 2 MiB WXU image using YOUR original dump.

Never executes a flash operation. Preserves the original upper 128 KiB verbatim,
including unknown factory, RF, KV and Tuya data. Replaces old BOOT/APP and OTA
areas and initializes a new vendor-compatible FDCM selector for sequence 0.
Do not publish the resulting image: preserved data may contain device secrets.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from make_ota_xz import BOOT_ID, section, package

FLASH_BYTES = 0x200000
OTA_METADATA = 0x13F000
OTA_STAGING = 0x140000
FACTORY_TAIL = 0x1E0000
SECTOR = 0x1000
APP_OFFSET = 0x8000


def digest(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def fdcm_seq0() -> bytes:
    """Exact ROM fdcm_rewrite() layout for 4-byte image_raw_cfg_t in 4096 bytes."""
    data_size = 4
    bitmap_size = (SECTOR - 8 - 1) // (data_size * 8 + 1) + 1
    assert bitmap_size == 124
    blob = bytearray(b"\xFF" * SECTOR)
    struct.pack_into("<IHH", blob, 0, 0xA55A5AA5, bitmap_size, data_size)
    blob[8] = 0xFE  # first FDCM record used; rest blank
    struct.pack_into("<HH", blob, 8 + bitmap_size, 0x5555, 0x9669)
    return bytes(blob)


def validate_fdcm_sector(data: bytes) -> bool:
    if len(data) != SECTOR:
        return False
    if struct.unpack_from("<IHH", data) != (0xA55A5AA5, 124, 4):
        return False
    if data[8] != 0xFE or data[9:132] != b"\xFF" * 123:
        return False
    if struct.unpack_from("<HH", data, 132) != (0x5555, 0x9669):
        return False
    return data[136:] == b"\xFF" * (SECTOR - 136)


def build(original: bytes, factory_firmware: bytes) -> tuple[bytes, dict]:
    if len(original) != FLASH_BYTES:
        raise ValueError("Original WXU dump must have exactly 2,097,152 bytes")
    old = section(original, 0, BOOT_ID)
    if old[9] != APP_OFFSET or old[12] != 0x177000 or (old[13] >> 16) != 400:
        raise ValueError("Unknown source layout: not the documented TRV16 WXU 2 MiB dump")
    new = section(factory_firmware, 0, BOOT_ID)
    if new[9] != APP_OFFSET or new[11] != 0x1000FF or new[12] != OTA_METADATA or (
        new[13] != ((640 << 16) | 1276)
    ):
        raise ValueError("New firmware is not the 640 KiB factory-preserving OTA layout")
    if len(factory_firmware) > OTA_METADATA:
        raise ValueError("Factory AWIH overlaps new FDCM metadata")
    # Full APP chain and XZ wrapper checks; never accept half-written firmware.
    _, info = package(factory_firmware)
    if not info["fits"]:
        raise ValueError("Firmware cannot fit the 640 KiB OTA slot")
    if info["app_end"] > OTA_METADATA:
        raise ValueError("Active APP overlaps FDCM")

    out = bytearray(b"\xFF" * FLASH_BYTES)
    out[:len(factory_firmware)] = factory_firmware
    out[OTA_METADATA:OTA_STAGING] = fdcm_seq0()
    out[FACTORY_TAIL:] = original[FACTORY_TAIL:]
    if not validate_fdcm_sector(out[OTA_METADATA:OTA_STAGING]):
        raise AssertionError("FDCM initial selector creation failed")
    if out[OTA_STAGING:FACTORY_TAIL] != b"\xFF" * (FACTORY_TAIL - OTA_STAGING):
        raise AssertionError("First OTA staging area is not erased")
    if out[FACTORY_TAIL:] != original[FACTORY_TAIL:]:
        raise AssertionError("Device-specific tail changed!")
    if out[:len(factory_firmware)] != factory_firmware:
        raise AssertionError("Firmware bytes changed!")
    manifest = {
        "status": "bench-only / requires UART flash + real readback",
        "source_sha256": digest(original),
        "new_awih_sha256": digest(factory_firmware),
        "result_sha256": digest(out),
        "flash_bytes": FLASH_BYTES,
        "active_start": APP_OFFSET,
        "fdcm_start": OTA_METADATA,
        "fdcm_bytes": SECTOR,
        "ota_start": OTA_STAGING,
        "ota_end_exclusive": FACTORY_TAIL,
        "ota_bytes": FACTORY_TAIL - OTA_STAGING,
        "preserved_tail_start": FACTORY_TAIL,
        "preserved_tail_sha256": digest(original[FACTORY_TAIL:]),
        "ota_package_bytes": info["ota_bytes"],
        "write_and_verify": [
            {"start": 0, "end_exclusive": OTA_STAGING,
             "sha256": digest(out[:OTA_STAGING])},
            {"start": OTA_STAGING, "end_exclusive": FACTORY_TAIL,
             "sha256": digest(out[OTA_STAGING:FACTORY_TAIL])},
            {"start": FACTORY_TAIL, "end_exclusive": FLASH_BYTES,
             "sha256": digest(out[FACTORY_TAIL:])},
        ],
    }
    return bytes(out), manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-dump", required=True, type=Path)
    parser.add_argument("--new-firmware", required=True, type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    data, manifest = build(args.original_dump.read_bytes(), args.new_firmware.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    manifest_path = args.output.with_suffix(".manifest.json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print("Generated offline migration image (NO hardware was flashed):", args.output)
    print("Offline manifest:", manifest_path)
    print("SHA-256:", manifest["result_sha256"])


if __name__ == "__main__":
    main()
