#!/usr/bin/env python3
"""Read-only XR806 AWIH/OTA-geometry inspection; never uploads or writes flash.

Usage: python tools/xr806/inspect_ota_geometry.py WXU_XR806_flash_2MiB.bin
All reported offsets are flash *file* offsets, not assumed live RAM addresses.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

HEADER = struct.Struct("<IIHH" + "I" * 13)
MAGIC = 0x48495741  # "AWIH" little endian
BOOT_ID = 0xA5FF5A00
SECTION_IDS = {
    0xA5FF5A00: "BOOT",
    0xA5FE5A01: "APP",
    0xA5FD5A02: "APP_XIP",
    0xA5FA5A05: "WLAN_BL",
    0xA5F95A06: "WLAN_FW",
    0xA5F85A07: "SYS_SDD",
}


def parse(data: bytes) -> dict:
    if len(data) < HEADER.size:
        raise ValueError("dump shorter than AWIH section header")
    boot = HEADER.unpack_from(data)
    if boot[0] != MAGIC or boot[10] != BOOT_ID:
        raise ValueError("flash start is not an expected XR806 AWIH BOOT header")
    private = boot[11:17]
    packed_ota = private[0]
    metadata_size = packed_ota >> 8
    metadata_flash_id = packed_ota & 0xFF
    metadata_start = private[1]
    image_max_kib = private[2] & 0xFFFF
    xz_max_kib = private[2] >> 16
    if metadata_size == 0 or metadata_start + metadata_size > len(data):
        raise ValueError("invalid boot-header OTA metadata range")
    if metadata_start % 4096 or metadata_size % 4096:
        raise ValueError("OTA metadata is not 4-KiB aligned")
    proposed_compressed_start = metadata_start + metadata_size
    proposed_compressed_end = proposed_compressed_start + xz_max_kib * 1024
    sections = []
    at = 0
    while True:
        at = data.find(b"AWIH", at)
        if at < 0:
            break
        if at + HEADER.size <= len(data):
            h = HEADER.unpack_from(data, at)
            if h[0] == MAGIC and h[10] in SECTION_IDS and h[4] < len(data):
                sections.append({
                    "offset": f"0x{at:06x}",
                    "kind": SECTION_IDS[h[10]],
                    "id": f"0x{h[10]:08x}",
                    "payload_bytes": h[4],
                    "next_relative": f"0x{h[9]:08x}",
                })
        at += 4
    return {
        "flash_bytes": len(data),
        "boot_header": {
            "ota_metadata_flash_id": f"0x{metadata_flash_id:02x}",
            "ota_metadata_start": f"0x{metadata_start:06x}",
            "ota_metadata_bytes": metadata_size,
            "image_max_kib": image_max_kib,
            "xz_max_kib": xz_max_kib,
        },
        "evidence": {
            "non_ff_bytes_at_sdk_example_0x100000_4k": sum(x != 255 for x in data[0x100000:0x101000]),
            "non_ff_bytes_at_header_metadata_4k": sum(
                x != 255 for x in data[metadata_start:metadata_start + metadata_size]
            ),
            "proposed_compressed_slot_start": f"0x{proposed_compressed_start:06x}",
            "proposed_compressed_slot_end": f"0x{proposed_compressed_end:06x}",
            "proposed_slot_in_dump_bounds": proposed_compressed_end <= len(data),
            "sections": sections,
        },
        "write_authorized": False,
        "warning": "Header decoding is not running-sequence, signature or rollback proof.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="local 2-MiB flash backup (read-only)")
    args = parser.parse_args()
    try:
        report = parse(args.dump.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"Invalid dump: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
