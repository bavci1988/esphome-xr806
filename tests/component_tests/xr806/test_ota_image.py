"""Tests for the XR806 BOOT header stamping and the compressed OTA image."""

from __future__ import annotations

import json
import lzma
import random
import struct

import pytest

from esphome.components.xr806.build_support.ota_image import (
    APP_ID,
    AWIH_MAGIC,
    BOOT_ID,
    HEADER,
    NO_NEXT,
    checksum16,
    make_ota_image,
    read_layout,
    read_section,
    stamp_boot_header,
)

APP_START = 0x8000
FDCM = 1276 * 1024
IMAGE_CFG = json.dumps(
    {
        "magic": "AWIH",
        "version": "0.5",
        "OTA": {"addr": "1276K", "size": "4K"},
        "image": {"max_size": "1276K", "xz_max_size": "640K"},
    }
)
SIZES = (640 << 16) | 1276


def awih(
    section_id: int, body: bytes, next_addr: int, private: list[int] | None = None
) -> bytes:
    fields = [AWIH_MAGIC, 5, 0, (0xFFFF - checksum16(body)) & 0xFFFF, len(body)]
    fields += [NO_NEXT, NO_NEXT, len(body), 1, next_addr, section_id]
    fields += private or [NO_NEXT] * 6
    fields[2] = (0xFFFF - checksum16(HEADER.pack(*fields))) & 0xFFFF
    return HEADER.pack(*fields) + body


def firmware(
    app_body: bytes = b"application" * 100, private: list[int] | None = None
) -> bytes:
    """A BOOT section and a two-section APP chain, as mkimage writes them."""
    boot = awih(
        BOOT_ID,
        b"boot",
        APP_START,
        private or [NO_NEXT, NO_NEXT, SIZES, NO_NEXT, NO_NEXT, NO_NEXT],
    )
    second = APP_START + (64 + len(app_body) + 0x1FFF) // 0x1000 * 0x1000
    app = awih(APP_ID, app_body, second)
    xip = awih(0xA5FD5A02, b"xip code" * 50, NO_NEXT)
    image = bytearray(b"\xff" * (second - APP_START + len(xip) + APP_START))
    image[: len(boot)] = boot
    image[APP_START : APP_START + len(app)] = app
    image[second:] = xip
    return bytes(image)


def stamped() -> bytes:
    return stamp_boot_header(firmware(), IMAGE_CFG)


def test_checksum16_pads_odd_length() -> None:
    assert checksum16(b"\x01\x02\x03") == 0x0201 + 0x03


def test_stamp_writes_rom_fields() -> None:
    raw = firmware()
    result = stamp_boot_header(raw, IMAGE_CFG)
    assert result[64:] == raw[64:]
    head = HEADER.unpack_from(result)
    assert head[11] == 0x1000FF
    assert head[12] == FDCM
    assert head[13] == SIZES
    assert checksum16(result[:64]) == 0xFFFF


def test_stamp_is_idempotent() -> None:
    once = stamped()
    assert stamp_boot_header(once, IMAGE_CFG) == once


def test_stamp_rejects_slot_reaching_reserved_tail() -> None:
    with pytest.raises(ValueError, match="reserved tail"):
        stamp_boot_header(firmware(), IMAGE_CFG.replace('"640K"', '"768K"'))


def test_stamp_rejects_other_layout() -> None:
    other = firmware(private=[NO_NEXT, 0x1337000, SIZES, NO_NEXT, NO_NEXT, NO_NEXT])
    with pytest.raises(ValueError, match="different OTA layout"):
        stamp_boot_header(other, IMAGE_CFG)


def test_read_layout_strips_comments() -> None:
    assert read_layout("/* vendor */" + IMAGE_CFG) == (FDCM, 4096, FDCM, 640 * 1024)


def test_ota_image_round_trip() -> None:
    image = stamped()
    ota, info = make_ota_image(image)
    fields = read_section(ota, 0, APP_ID)
    assert fields[8] == 0x11  # compressed APP
    assert fields[9] == NO_NEXT
    assert info.sections == 2
    assert info.ota_bytes == len(ota)
    assert info.slot_bytes == 640 * 1024
    assert lzma.decompress(ota[64:]) == image[APP_START : APP_START + info.raw_bytes]


def test_ota_image_uses_bootloader_dictionary() -> None:
    ota, _ = make_ota_image(stamped())
    # First XZ block header after the 12-byte stream header: LZMA2 filter (0x21) with one
    # property byte; 2 means an 8 KiB dictionary, the bootloader's limit.
    block = ota[64 + 12 :]
    assert block[2:5] == bytes([0x21, 0x01, 0x02])


@pytest.mark.parametrize("offset", [12, APP_START + 70])
def test_ota_image_rejects_corruption(offset: int) -> None:
    image = bytearray(stamped())
    image[offset] ^= 3
    with pytest.raises(ValueError):
        make_ota_image(bytes(image))


def test_ota_image_rejects_unstamped_firmware() -> None:
    with pytest.raises(ValueError, match="FDCM"):
        make_ota_image(firmware())


def test_ota_image_rejects_oversize() -> None:
    body = random.Random(20261109).randbytes(700 * 1024)
    image = stamp_boot_header(firmware(body), IMAGE_CFG)
    with pytest.raises(ValueError, match="does not fit"):
        make_ota_image(image)


def test_ota_image_rejects_backward_chain() -> None:
    image = bytearray(stamped())
    fields = list(HEADER.unpack_from(image, APP_START))
    fields[9] = APP_START + 16
    fields[2] = 0
    fields[2] = (0xFFFF - checksum16(HEADER.pack(*fields))) & 0xFFFF
    struct.pack_into(HEADER.format, image, APP_START, *fields)
    with pytest.raises(ValueError, match="next_addr"):
        make_ota_image(bytes(image))
