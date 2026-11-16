"""Tests for the XR806 WLAN firmware keepalive patch."""

import hashlib
from pathlib import Path
import struct

import pytest

from esphome.components.xr806.build_support import wlan_fw_patch as patch_mod

SECTION = 0x09009C00
SECTION_SIZE = 0x1000


def _image() -> bytes:
    """A firmware image with one load record that holds the stock code at the patched addresses."""
    data = bytearray(SECTION_SIZE)
    for address, code in (
        (patch_mod.IMMEDIATE_SEND_ADDRESS, patch_mod.IMMEDIATE_SEND_STOCK),
        (patch_mod.PAYLOAD_SOURCE_ADDRESS, patch_mod.PAYLOAD_SOURCE_STOCK),
        (patch_mod.PEER_DATA_ADDRESS, patch_mod.PEER_DATA_STOCK),
    ):
        data[address - SECTION : address - SECTION + len(code)] = code
    image = bytearray(b"XR01")
    image += struct.pack("<3I", 0, SECTION, SECTION_SIZE) + data
    image += struct.pack("<3I", 1, SECTION, 0) + struct.pack("<I", 0)
    image += struct.pack("<3I", 4, SECTION, 0)
    words = struct.unpack(f"<{len(image) // 4}I", image)
    struct.pack_into("<I", image, len(image) - 4, (-sum(words[:-1])) & 0xFFFFFFFF)
    return bytes(image)


def _at(image: bytes, address: int, length: int) -> bytes:
    offset = 4 + 12 + address - SECTION
    return image[offset : offset + length]


def test_thumb_bl_encoding() -> None:
    # bl 0x0900A3C8 at 0x09009E0C, as assembled by GNU as
    assert patch_mod.thumb_bl(0x09009E0C, 0x0900A3C8) == bytes.fromhex("00f0dcfa")
    # bl 0x0900A6C8 at 0x0900A374, the stock instruction
    assert patch_mod.thumb_bl(0x0900A374, 0x0900A6C8) == patch_mod.IMMEDIATE_SEND_STOCK


def test_patch_changes_the_three_places_and_keeps_the_checksum() -> None:
    stock = _image()
    patched = patch_mod.patch(stock, verify_hash=False)

    assert len(patched) == len(stock)
    assert sum(struct.unpack(f"<{len(patched) // 4}I", patched)) & 0xFFFFFFFF == 0
    assert _at(patched, patch_mod.IMMEDIATE_SEND_ADDRESS, 4) == patch_mod.THUMB_NOP * 2
    assert (
        _at(patched, patch_mod.PAYLOAD_SOURCE_ADDRESS, 6)
        == bytes.fromhex("00f0dcfa") + patch_mod.THUMB_NOP
    )
    assert _at(patched, patch_mod.CAVE_ADDRESS, len(patch_mod.CAVE)) == patch_mod.CAVE
    assert _at(patched, patch_mod.PEER_DATA_ADDRESS, 2) == patch_mod.PEER_DATA_PATCHED


def test_patch_refuses_other_code() -> None:
    stock = bytearray(_image())
    offset = 4 + 12 + patch_mod.IMMEDIATE_SEND_ADDRESS - SECTION
    stock[offset] ^= 0xFF
    with pytest.raises(patch_mod.WlanFirmwarePatchError, match="unexpected code"):
        patch_mod.patch(bytes(stock), verify_hash=False)


def test_patch_refuses_an_unknown_firmware() -> None:
    with pytest.raises(
        patch_mod.WlanFirmwarePatchError, match="not the known stock firmware"
    ):
        patch_mod.patch(_image())


def test_write_patched_keeps_an_identical_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(patch_mod, "STOCK_SHA256", hashlib.sha256(_image()).hexdigest())
    stock = tmp_path / "wlan_fw.bin"
    stock.write_bytes(_image())
    output = tmp_path / "out" / "wlan_fw_keepalive.bin"

    patch_mod.write_patched(stock, output)
    mtime = output.stat().st_mtime_ns
    patch_mod.write_patched(stock, output)

    assert output.stat().st_mtime_ns == mtime
    assert output.read_bytes() == patch_mod.patch(_image(), verify_hash=False)
