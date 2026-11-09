#!/usr/bin/env python3
"""Stamp vendor mkimage AWIH BOOT header with the SDK OTA layout.

mkimage writes image.max_size and image.xz_max_size into BOOT priv[2], but
does not populate BOOT priv[0]/priv[1] from the OTA object in this invocation.
XRadio's ROM image_init() reads those fields, so a successful image build is
NOT evidence that OTA is enabled. Fail closed rather than guessing geometry.
This tool only edits an offline file; it never accesses device flash.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct

from make_ota_xz import BOOT_ID, HEADER, checksum16, section

FLASH_BYTES = 2 * 1024 * 1024
FACTORY_RESERVED_START = 0x1E0000
BOOT_APP_OFFSET = 0x8000
SECTOR_BYTES = 4096


def parse_size(value: str) -> int:
    match = re.fullmatch(r"(\d+)([KkMm]?)", value)
    if match is None:
        raise ValueError(f"Invalid image.cfg size: {value!r}")
    factor = {"": 1, "K": 1024, "k": 1024, "M": 1024 * 1024, "m": 1024 * 1024}[match[2]]
    return int(match[1]) * factor


def read_geometry(cfg_text: str) -> tuple[int, int, int, int]:
    clean = re.sub(r"/\*.*?\*/", "", cfg_text, flags=re.S)
    cfg = json.loads(clean)
    if cfg["magic"] != "AWIH" or cfg["version"] != "0.5":
        raise ValueError("Unexpected vendor image format")
    ota = cfg.get("OTA")
    image = cfg.get("image")
    if not ota or not image:
        raise ValueError("OTA and image geometry required in preprocessed image.cfg")
    metadata = parse_size(ota["addr"])
    metadata_size = parse_size(ota["size"])
    max_total = parse_size(image["max_size"])
    compressed_size = parse_size(image["xz_max_size"])
    if (metadata_size != SECTOR_BYTES or metadata != max_total or
        metadata % SECTOR_BYTES or max_total % 1024 or
        compressed_size % 1024 or compressed_size > 0xFFFF * 1024 or
        metadata + metadata_size + compressed_size > FACTORY_RESERVED_START or
        metadata + metadata_size + compressed_size > FLASH_BYTES or
        max_total < BOOT_APP_OFFSET + SECTOR_BYTES):
        raise ValueError("Unsafe OTA geometry: would overlap active image or reserved factory tail")
    return metadata, metadata_size, max_total, compressed_size


def stamp(firmware: bytes, image_cfg: str) -> bytes:
    metadata, size, image_max, compressed = read_geometry(image_cfg)
    section(firmware, 0, BOOT_ID)
    values = list(HEADER.unpack_from(firmware))
    if values[9] != BOOT_APP_OFFSET or len(firmware) > metadata:
        raise ValueError("Unexpected BOOT->APP offset or active image exceeds OTA metadata")
    if values[13] != ((compressed // 1024) << 16) | (image_max // 1024):
        raise ValueError("Vendor image/max XZ geometry does not match the image.cfg values")
    expected_flash_size = (size << 8) | 0xFF  # inherit flash 0; OTA size in bytes
    if values[11] not in (0xFFFFFFFF, expected_flash_size) or values[12] not in (0xFFFFFFFF, metadata):
        raise ValueError("Refusing to overwrite unrelated vendor BOOT OTA geometry")
    values[11] = expected_flash_size
    values[12] = metadata
    values[2] = 0
    first = HEADER.pack(*values)
    values[2] = (0xFFFF - checksum16(first)) & 0xFFFF
    out = HEADER.pack(*values) + firmware[64:]
    section(out, 0, BOOT_ID)
    if HEADER.unpack_from(out)[11:14] != (expected_flash_size, metadata, values[13]):
        raise AssertionError("BOOT OTA geometry did not roundtrip")
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--image-config", type=Path, required=True)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    output = stamp(args.firmware.read_bytes(), args.image_config.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temp = args.output.with_name(args.output.name + ".stamping")
    try:
        temp.write_bytes(output)
        temp.replace(args.output)
    finally:
        temp.unlink(missing_ok=True)
    metadata, size, app_max, xz = read_geometry(args.image_config.read_text())
    print(f"[XR806] stamped BOOT: FDCM=0x{metadata:X}+{size}, "
          f"app_limit=0x{app_max:X}, XZ_slot={xz}, reserved_tail=0x{FACTORY_RESERVED_START:X}")


if __name__ == "__main__":
    main()
