#!/usr/bin/env python3
"""Make a vendor XRadio AWIH/XZ OTA image from an SDK-built AWIH firmware.

For the 2 MiB WXU compression bootloader, which expands the compressed staging
slot into the active application on reboot. Requires the matching bootloader.
No flash IO. Rejects invalid AWIH sections and over-capacity OTA images.
"""
from __future__ import annotations

import argparse
import lzma
from pathlib import Path
import struct

HEADER = struct.Struct("<IIHH7I6I")
AWIH_MAGIC = 0x48495741
BOOT_ID = 0xA5FF5A00
APP_ID = 0xA5FE5A01
BOOTLOADER_XZ_DICT = 8192  # project/bootloader/main.c: BL_DEC_IMG_DICT_MAX


def checksum16(data: bytes) -> int:
    """rom_image_checksum16: little-endian 16-bit words modulo 65536."""
    return sum(data[i] | ((data[i + 1] << 8) if i + 1 < len(data) else 0)
               for i in range(0, len(data), 2)) & 0xFFFF


def section(data: bytes, offset: int, expected: int | None = None) -> tuple:
    if offset < 0 or offset + 64 > len(data):
        raise ValueError(f"AWIH header out of bounds at 0x{offset:X}")
    fields = HEADER.unpack_from(data, offset)
    magic, ver, head_sum, body_sum, size = fields[:5]
    successor, section_id = fields[9:11]
    if magic != AWIH_MAGIC or ver != 5 or checksum16(data[offset:offset + 64]) != 0xFFFF:
        raise ValueError(f"Invalid AWIH header checksum/magic at 0x{offset:X}")
    if expected is not None and section_id != expected:
        raise ValueError(f"Unexpected AWIH section at 0x{offset:X}: {section_id:08X}")
    if not size or offset + 64 + size > len(data):
        raise ValueError(f"Invalid AWIH body length at 0x{offset:X}")
    if (checksum16(data[offset + 64:offset + 64 + size]) + body_sum) & 0xFFFF != 0xFFFF:
        raise ValueError(f"AWIH body checksum mismatch at 0x{offset:X}")
    return fields


def package(firmware: bytes, *, flash_size: int = 2 * 1024 * 1024,
            allow_oversize: bool = False) -> tuple[bytes, dict]:
    boot = section(firmware, 0, BOOT_ID)
    app_start = boot[9]
    private = boot[11:]
    metadata = private[1]
    max_total_kib = private[2] & 0xFFFF
    xz_kib = private[2] >> 16
    if not app_start or app_start == 0xFFFFFFFF or app_start % 4096:
        raise ValueError("Bad bootloader/application boundary")
    if metadata % 4096 or metadata <= app_start or metadata >= flash_size:
        raise ValueError("Invalid OTA metadata location")
    if not xz_kib or metadata + 4096 + (xz_kib * 1024) > flash_size:
        raise ValueError("Compressed slot outside physical flash")
    max_raw_bytes = max_total_kib * 1024 - app_start
    if max_raw_bytes <= 0 or app_start + max_raw_bytes > metadata:
        raise ValueError("Raw-image max size overlaps FDCM")

    cursor = app_start
    visited = set()
    sections = 0
    while True:
        if cursor in visited or cursor >= metadata:
            raise ValueError("Loop or invalid section address")
        visited.add(cursor)
        sh = section(firmware, cursor, APP_ID if sections == 0 else None)
        sections += 1
        if sections > 32:
            raise ValueError("Excessive AWIH section count")
        end = cursor + 64 + sh[4]
        if end > metadata or end > app_start + max_raw_bytes:
            raise ValueError("Firmware payload overlaps OTA area")
        successor = sh[9]
        if successor == 0xFFFFFFFF:
            break
        if successor < end or successor % 4:
            raise ValueError("Invalid AWIH next_addr")
        cursor = successor
    raw = firmware[app_start:end]
    xz = lzma.compress(raw, format=lzma.FORMAT_XZ,
                       filters=[{"id": lzma.FILTER_LZMA2,
                                 "dict_size": BOOTLOADER_XZ_DICT, "preset": 6}])
    fields = [AWIH_MAGIC, 5, 0, (0xFFFF - checksum16(xz)) & 0xFFFF,
              len(xz), 0xFFFFFFFF, 0xFFFFFFFF, len(xz), 0x11,
              0xFFFFFFFF, APP_ID] + [0xFFFFFFFF] * 6
    hdr = HEADER.pack(*fields)
    fields[2] = (0xFFFF - checksum16(hdr)) & 0xFFFF
    hdr = HEADER.pack(*fields)
    output = hdr + xz
    slot_bytes = xz_kib * 1024
    meta = {"sections": sections, "app_start": app_start, "app_end": end,
            "raw_bytes": len(raw), "ota_bytes": len(output),
            "ota_slot_bytes": slot_bytes, "metadata_addr": metadata,
            "fits": len(output) <= slot_bytes}
    if not meta["fits"] and not allow_oversize:
        raise ValueError(f"OTA package {len(output)} bytes > {slot_bytes} byte slot; "
                         "cannot safely upload. Rebuild for a suitable flash layout.")
    if checksum16(hdr) != 0xFFFF or (checksum16(xz) + fields[3]) & 0xFFFF != 0xFFFF:
        raise AssertionError("AWIH checksum construction failed")
    if lzma.decompress(xz) != raw:
        raise AssertionError("Compressed image is not bit-identical")
    return output, meta


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--flash-size", type=lambda x: int(x, 0), default=2 * 1024 * 1024)
    parser.add_argument("--allow-oversize-for-analysis", action="store_true",
                        help="DO NOT upload the resulting oversized package")
    args = parser.parse_args()
    image, layout = package(args.firmware.read_bytes(), flash_size=args.flash_size,
                            allow_oversize=args.allow_oversize_for_analysis)
    for k, v in layout.items():
        print(f"{k}={v}")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
        print(f"OTA image: {args.output}")


if __name__ == "__main__":
    main()
