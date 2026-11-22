"""Tests for the XR806 WLAN firmware keepalive patch."""

import hashlib
from pathlib import Path
import shutil
import struct

import pytest

from esphome.components.xr806.build_support import wlan_fw_patch as patch_mod

SECTION = 0x09009C00
SECTION_SIZE = 0x1000
# Stands in for the frame chooser; ends with the literal the peer data code reaches.
CAVE = bytes(range(0x20, 0x20 + 88)) + struct.pack("<I", patch_mod.CAVE_LITERAL)
# wlan_fw_keepalive.S as assembled with GNU as and checked in Unicorn against the firmware's data layout.
CAVE_CHECKED = bytes.fromhex(
    "24e0ffe7059800281cd1707a06281bd130780007000f052816d3b079800613d1f079002810d1687b8006800e"
    "02280bd1a8780002e97808432049884202d02049884201d1069870470120704770b51199ca88139b002b2ed1"
    "002a2cd0194c2468194d2d68641b194d2e78013e04d32e7002d1013e033d2e7000258026964202d3b61aad18"
    "fae70026ac4206d3641b0136052ef9d90526ac1a05e02300934201d39b1afbe7e41a032e01d3033e08498423"
    "7343c9180919083170bd000050000000a00c000004bc000998bb000973bc0009fcbd0009"
)


def _image() -> bytes:
    """A firmware image with one load record that holds the stock code at the patched addresses."""
    data = bytearray(SECTION_SIZE)
    for address, code in (
        (patch_mod.IMMEDIATE_SEND_ADDRESS, patch_mod.IMMEDIATE_SEND_STOCK),
        (patch_mod.PAYLOAD_SOURCE_ADDRESS, patch_mod.PAYLOAD_SOURCE_STOCK),
        (patch_mod.PEER_DATA_ADDRESS, patch_mod.PEER_DATA_STOCK),
        (patch_mod.FILTER_DROP_ADDRESS, patch_mod.FILTER_DROP_STOCK),
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


def test_patch_changes_the_code_and_keeps_the_checksum() -> None:
    stock = _image()
    patched = patch_mod.patch(stock, CAVE, verify_hash=False)

    assert len(patched) == len(stock)
    assert sum(struct.unpack(f"<{len(patched) // 4}I", patched)) & 0xFFFFFFFF == 0
    assert _at(patched, patch_mod.IMMEDIATE_SEND_ADDRESS, 4) == patch_mod.THUMB_NOP * 2
    # bl 0x0900A3CA at 0x0900A216, as assembled by GNU as
    assert _at(patched, patch_mod.FILTER_DROP_ADDRESS, 8) == bytes.fromhex(
        "00f0d8f8"
    ) + (patch_mod.THUMB_NOP * 2)
    assert (
        _at(patched, patch_mod.PAYLOAD_SOURCE_ADDRESS, 6)
        == bytes.fromhex("00f0dcfa") + patch_mod.THUMB_NOP
    )
    assert _at(patched, patch_mod.CAVE_ADDRESS, len(CAVE)) == CAVE
    # ldr r2,=0x0900BC73; subs r2,#0x62; ldrb r2,[r2]; str r2,[sp]; movs r1,r0; movs r0,r4; bl 0x0900A720;
    # ldr r1,[sp,#8]; cmp r1,#0; beq 0x0900A15E; ldr r0,[sp]; cmp r0,#1; bne 0x0900A220; nop
    peer_data = _at(
        patched, patch_mod.PEER_DATA_ADDRESS, len(patch_mod.PEER_DATA_STOCK)
    )
    assert peer_data[2:] == bytes.fromhex(
        "623a127800920100200000f0e8fa0299002903d00098012861d1c046"
    )
    literal = ((patch_mod.PEER_DATA_ADDRESS + 4) & ~3) + 4 * peer_data[0]
    assert peer_data[1] == 0x4A
    assert _at(patched, literal, 4) == struct.pack("<I", patch_mod.CAVE_LITERAL)


def test_patch_refuses_other_code() -> None:
    stock = bytearray(_image())
    offset = 4 + 12 + patch_mod.IMMEDIATE_SEND_ADDRESS - SECTION
    stock[offset] ^= 0xFF
    with pytest.raises(patch_mod.WlanFirmwarePatchError, match="unexpected code"):
        patch_mod.patch(bytes(stock), CAVE, verify_hash=False)


def test_patch_refuses_an_unknown_firmware() -> None:
    with pytest.raises(
        patch_mod.WlanFirmwarePatchError, match="not the known stock firmware"
    ):
        patch_mod.patch(_image(), CAVE)


def test_write_patched_keeps_an_identical_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(patch_mod, "STOCK_SHA256", hashlib.sha256(_image()).hexdigest())
    monkeypatch.setattr(patch_mod, "assemble_cave", lambda toolchain, work: CAVE)
    stock = tmp_path / "wlan_fw.bin"
    stock.write_bytes(_image())
    output = tmp_path / "out" / "wlan_fw_keepalive.bin"

    patch_mod.write_patched(stock, output, tmp_path)
    mtime = output.stat().st_mtime_ns
    patch_mod.write_patched(stock, output, tmp_path)

    assert output.stat().st_mtime_ns == mtime
    assert output.read_bytes() == patch_mod.patch(_image(), CAVE, verify_hash=False)


@pytest.mark.skipif(
    shutil.which("arm-none-eabi-as") is None, reason="needs the ARM toolchain"
)
def test_frame_chooser_assembles_to_the_checked_code(tmp_path: Path) -> None:
    toolchain = Path(shutil.which("arm-none-eabi-as")).parent

    assert patch_mod.assemble_cave(toolchain, tmp_path) == CAVE_CHECKED
