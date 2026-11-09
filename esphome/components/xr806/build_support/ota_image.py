"""XRadio AWIH images for XR806 network updates.

The vendor ``mkimage`` tool does not fill in the OTA fields of the BOOT header,
so `stamp_boot_header` adds them from ``image.cfg``. The ROM reads them to find
the boot selection sector (FDCM) and the compressed update slot.

`make_ota_image` builds the file a network update sends: the APP section chain
of the firmware, compressed with XZ into one AWIH section. The bootloader
decompresses it into the active image area on the next boot.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import lzma
import re
import struct

HEADER = struct.Struct("<IIHH7I6I")
AWIH_MAGIC = 0x48495741
AWIH_VERSION = 5
BOOT_ID = 0xA5FF5A00
APP_ID = 0xA5FE5A01
NO_NEXT = 0xFFFFFFFF
# project/bootloader/main.c: BL_DEC_IMG_DICT_MAX
BOOTLOADER_XZ_DICT = 8 * 1024
MAX_SECTIONS = 32
SECTOR_BYTES = 4096
FLASH_BYTES = 2 * 1024 * 1024
# Data at the end of the original WXU flash that is not identified yet.
RESERVED_TAIL_START = 0x1E0000
BOOT_APP_OFFSET = 0x8000

# Header field indexes in HEADER
_MAGIC, _VERSION, _HEAD_SUM, _DATA_SUM, _DATA_SIZE = range(5)
_ATTRIBUTE, _NEXT, _ID, _PRIV0, _PRIV1, _PRIV2 = 8, 9, 10, 11, 12, 13
_ATTR_XZ_APP = 0x11


@dataclass(frozen=True)
class OTAImageInfo:
    sections: int
    raw_bytes: int
    ota_bytes: int
    slot_bytes: int


def checksum16(data: bytes) -> int:
    """The ROM image checksum: sum of little-endian 16-bit words, modulo 65536."""
    if len(data) % 2:
        data += b"\x00"
    return sum(struct.unpack(f"<{len(data) // 2}H", data)) & 0xFFFF


def _pack_header(fields: list[int]) -> bytes:
    fields[_HEAD_SUM] = 0
    fields[_HEAD_SUM] = (0xFFFF - checksum16(HEADER.pack(*fields))) & 0xFFFF
    return HEADER.pack(*fields)


def read_section(
    data: bytes, offset: int, expected_id: int | None = None
) -> tuple[int, ...]:
    """Return the header fields of the AWIH section at offset after checking it."""
    if offset < 0 or offset + HEADER.size > len(data):
        raise ValueError(f"AWIH header out of bounds at 0x{offset:X}")
    fields = HEADER.unpack_from(data, offset)
    if (
        fields[_MAGIC] != AWIH_MAGIC
        or fields[_VERSION] != AWIH_VERSION
        or checksum16(data[offset : offset + HEADER.size]) != 0xFFFF
    ):
        raise ValueError(f"Invalid AWIH header at 0x{offset:X}")
    if expected_id is not None and fields[_ID] != expected_id:
        raise ValueError(f"Unexpected AWIH section 0x{fields[_ID]:08X} at 0x{offset:X}")
    size = fields[_DATA_SIZE]
    body = offset + HEADER.size
    if not size or body + size > len(data):
        raise ValueError(f"Invalid AWIH body length at 0x{offset:X}")
    if (checksum16(data[body : body + size]) + fields[_DATA_SUM]) & 0xFFFF != 0xFFFF:
        raise ValueError(f"AWIH body checksum mismatch at 0x{offset:X}")
    return fields


def _parse_size(value: str) -> int:
    if (match := re.fullmatch(r"(\d+)([KkMm]?)", value)) is None:
        raise ValueError(f"Invalid image.cfg size: {value!r}")
    return int(match[1]) * {"": 1, "k": 1024, "m": 1024 * 1024}[match[2].lower()]


def read_layout(image_cfg: str) -> tuple[int, int, int, int]:
    """Return (FDCM address, FDCM size, image limit, XZ slot size) from image.cfg."""
    cfg = json.loads(re.sub(r"/\*.*?\*/", "", image_cfg, flags=re.DOTALL))
    if cfg.get("magic") != "AWIH" or cfg.get("version") != "0.5":
        raise ValueError("Unexpected vendor image format")
    if not (ota := cfg.get("OTA")) or not (image := cfg.get("image")):
        raise ValueError("image.cfg has no OTA or image layout")
    metadata = _parse_size(ota["addr"])
    metadata_size = _parse_size(ota["size"])
    image_limit = _parse_size(image["max_size"])
    slot = _parse_size(image["xz_max_size"])
    if metadata_size != SECTOR_BYTES or metadata % SECTOR_BYTES:
        raise ValueError("The FDCM sector must be one aligned 4 KiB sector")
    if metadata != image_limit or image_limit < BOOT_APP_OFFSET + SECTOR_BYTES:
        raise ValueError("The image limit must end at the FDCM sector")
    if slot % 1024 or slot > 0xFFFF * 1024:
        raise ValueError("Invalid XZ slot size")
    if metadata + metadata_size + slot > RESERVED_TAIL_START:
        raise ValueError("Unsafe OTA layout: the XZ slot reaches the reserved tail")
    return metadata, metadata_size, image_limit, slot


def stamp_boot_header(firmware: bytes, image_cfg: str) -> bytes:
    """Write the OTA layout from image.cfg into the BOOT header. Idempotent."""
    metadata, metadata_size, image_limit, slot = read_layout(image_cfg)
    fields = list(read_section(firmware, 0, BOOT_ID))
    if fields[_NEXT] != BOOT_APP_OFFSET or len(firmware) > metadata:
        raise ValueError(
            "Unexpected BOOT->APP offset, or the image reaches the FDCM sector"
        )
    if fields[_PRIV2] != ((slot // 1024) << 16) | (image_limit // 1024):
        raise ValueError("BOOT image size fields do not match image.cfg")
    flash_and_size = (metadata_size << 8) | 0xFF  # flash 0xFF: same flash as the image
    if fields[_PRIV0] not in (NO_NEXT, flash_and_size) or fields[_PRIV1] not in (
        NO_NEXT,
        metadata,
    ):
        raise ValueError("BOOT header already holds a different OTA layout")
    fields[_PRIV0] = flash_and_size
    fields[_PRIV1] = metadata
    result = _pack_header(fields) + firmware[HEADER.size :]
    read_section(result, 0, BOOT_ID)
    return result


def make_ota_image(
    firmware: bytes, *, flash_size: int = FLASH_BYTES
) -> tuple[bytes, OTAImageInfo]:
    """Compress the APP section chain of a stamped firmware into an AWIH/XZ update."""
    boot = read_section(firmware, 0, BOOT_ID)
    app_start = boot[_NEXT]
    metadata = boot[_PRIV1]
    image_limit = (boot[_PRIV2] & 0xFFFF) * 1024
    slot = (boot[_PRIV2] >> 16) * 1024
    if not app_start or app_start == NO_NEXT or app_start % SECTOR_BYTES:
        raise ValueError("Bad bootloader/application boundary")
    if metadata % SECTOR_BYTES or metadata <= app_start or metadata >= flash_size:
        raise ValueError("Invalid FDCM address in BOOT header")
    if not slot or metadata + SECTOR_BYTES + slot > flash_size:
        raise ValueError("Compressed slot outside physical flash")
    if image_limit <= app_start or image_limit > metadata:
        raise ValueError("Image limit overlaps FDCM")

    cursor = app_start
    sections = 0
    while True:
        fields = read_section(firmware, cursor, APP_ID if sections == 0 else None)
        sections += 1
        if sections > MAX_SECTIONS:
            raise ValueError("Too many AWIH sections")
        end = cursor + HEADER.size + fields[_DATA_SIZE]
        if end > image_limit:
            raise ValueError("Firmware does not fit the active image area")
        if (successor := fields[_NEXT]) == NO_NEXT:
            break
        if successor < end or successor % 4:
            raise ValueError("Invalid AWIH next_addr")
        cursor = successor

    raw = firmware[app_start:end]
    payload = lzma.compress(
        raw,
        format=lzma.FORMAT_XZ,
        check=lzma.CHECK_CRC32,
        filters=[
            {"id": lzma.FILTER_LZMA2, "dict_size": BOOTLOADER_XZ_DICT, "preset": 6}
        ],
    )
    fields = [
        AWIH_MAGIC,
        AWIH_VERSION,
        0,
        (0xFFFF - checksum16(payload)) & 0xFFFF,
        len(payload),
    ]
    fields += [NO_NEXT, NO_NEXT, len(payload), _ATTR_XZ_APP, NO_NEXT, APP_ID] + [
        NO_NEXT
    ] * 6
    output = _pack_header(fields) + payload
    info = OTAImageInfo(sections, len(raw), len(output), slot)
    if len(output) > slot:
        raise ValueError(
            f"OTA image of {len(output)} bytes does not fit the {slot} byte update slot"
        )
    read_section(output, 0, APP_ID)
    if lzma.decompress(payload) != raw:
        raise ValueError("XZ round trip changed the image")
    return output, info
