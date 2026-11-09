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


def awih_checksum16(data: bytes) -> int:
    """Vendor image_checksum16(): sum of LE 16-bit words modulo 65536."""
    result = 0
    for offset in range(0, len(data) - 1, 2):
        result += data[offset] | (data[offset + 1] << 8)
    if len(data) & 1:
        result += data[-1]
    return result & 0xFFFF


def decode_fdcm_selection(data: bytes, start: int, size: int) -> dict:
    """Decode the vendor ROM's append-only 4-byte FDCM image selection records.

    Reconstructs src/rom/rom_bin/src/image/fdcm.c and image/image.c. This is
    offline evidence, not proof that a bootloader on the board uses that ROM.
    """
    region = data[start:start + size]
    if len(region) < 12:
        raise ValueError("OTA FDCM area too small")
    magic, bitmap_size, data_size = struct.unpack_from("<IHH", region)
    if magic != 0xA55A5AA5 or data_size != 4 or bitmap_size == 0 or (
        8 + bitmap_size >= len(region)
    ):
        raise ValueError("invalid OTA FDCM header")
    bitmap = region[8:8 + bitmap_size]
    used = 0
    for value in bitmap:
        if value == 0:
            used += 8
            continue
        remaining = (~value) & 0xFF
        while remaining:
            remaining >>= 1
            used += 1
        break
    if used == 0 or 8 + bitmap_size + used * data_size > len(region):
        raise ValueError("OTA FDCM records absent or exceed area")
    records = []
    for i in range(used):
        raw_seq, raw_state = struct.unpack_from("<HH", region, 8 + bitmap_size + i * data_size)
        seq = {0x5555: 0, 0xAAAA: 1}.get(raw_seq)
        state = {0x9669: "VERIFIED", 0x6996: "UNVERIFIED"}.get(raw_state, "UNKNOWN")
        records.append({"seq": seq, "state": state,
                        "offset": f"0x{start + 8 + bitmap_size + i * 4:06x}"})
    latest = records[-1]
    return {
        "record_count": len(records),
        "records": records,
        "latest_selected_seq": latest["seq"] if latest["state"] == "VERIFIED" else None,
    }


def parse(data: bytes) -> dict:
    if len(data) < HEADER.size:
        raise ValueError("dump shorter than AWIH section header")
    boot = HEADER.unpack_from(data)
    if boot[0] != MAGIC or boot[10] != BOOT_ID:
        raise ValueError("flash start is not an expected XR806 AWIH BOOT header")
    if awih_checksum16(data[:HEADER.size]) != 0xFFFF:
        raise ValueError("invalid vendor AWIH BOOT header checksum")
    boot_payload_end = HEADER.size + boot[4]
    if boot_payload_end > len(data) or (
        (boot[3] + awih_checksum16(data[HEADER.size:boot_payload_end])) & 0xFFFF
    ) != 0xFFFF:
        raise ValueError("invalid vendor AWIH BOOT payload checksum")
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
            payload_end = at + HEADER.size + h[4]
            if (h[0] == MAGIC and h[10] in SECTION_IDS and
                    payload_end <= len(data) and
                    awih_checksum16(data[at:at + HEADER.size]) == 0xFFFF):
                valid_payload = (
                    (h[3] + awih_checksum16(data[at + HEADER.size:payload_end])) & 0xFFFF
                ) == 0xFFFF
                sections.append({
                    "offset": f"0x{at:06x}",
                    "kind": SECTION_IDS[h[10]],
                    "id": f"0x{h[10]:08x}",
                    "payload_bytes": h[4],
                    "next_relative": f"0x{h[9]:08x}",
                    "header_checksum_valid": True,
                    "payload_checksum_valid": valid_payload,
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
        "fdcm_selection": decode_fdcm_selection(data, metadata_start, metadata_size),
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
        "warning": "AWIH 16-bit checksums detect accidental corruption, NOT cryptographic authenticity; physical running slot, signature and rollback remain unproven.",
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
