"""Patch of the XR806 WLAN firmware (wlan_fw.bin) for the keepalive frame list.

wlan_fw.bin is a patch for the ROM of the WLAN core. It is a list of records after the magic "XR01":
``{0, address, length, data}`` loads data, ``{1, entry, 0}`` ends a group and is followed by a 4-byte group header,
``{2, length, data}`` is a register write list and ``{4, address, checksum}`` ends the file. The checksum makes the
sum of all 32-bit words of the file zero.

The keepalive ("P2P server", MIBs 0x1076..0x1079) is implemented in the patch. This module changes it so that
server 0 sends a different, pre-encrypted frame each time Home Assistant acknowledged the previous one:

1. MIB 0x1078 no longer sends a frame right away (BL at 0x0900A374), so the first frame goes out one period after
   configuration instead of while the host suspends the WLAN.
2. Before the payload copy at 0x09009E0C a new function chooses frame k = IP ID - IPIdInit from the payload areas
   of the three keepalive entries (128 // length frames each). It is placed at 0x0900A3C8, in the UDP keepalive
   (0x0900A3C6), which is only used for servers with the UDP flag; ESPHome uses TCP only.
3. The receive filter (0x0900A012) acknowledged data from the peer itself and dropped it, so a command from Home
   Assistant never reached the host. Data segments are now passed to the host, which wakes it (0x0900A14C).

Only the firmware with the known SHA-256 is patched; see docs/WLAN_FIRMWARE_NOTES.md in the xr806-sdk repository.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct

STOCK_SHA256 = "0d08b6c6d0c9bcf3af7b3ebdab55cb623f28feb8392343585255be17c98e59c8"

# Frame chooser, Thumb (ARMv5TE), linked at CAVE_ADDRESS. Source:
#   push {r4-r6,lr}; ldr r1,[sp,#0x44]; ldrh r2,[r1,#6]; ldr r3,[sp,#0x4c]; cmp r3,#0; bne plain
#   r4 = (u16)(*0x0900BC0C - *(u16 *)0x0900BB96)      ; k = IP ID - IPIdInit of server 0
#   r5 = 128 / r2 (by subtraction); r6 = r4 / r5, r4 = r4 % r5; if r6 > 2: r6 = 2, r4 = r5 - 1
#   r1 += r6 * 0x84 + r4 * r2
#   plain: r1 += 8; pop {r4-r6,pc}
CAVE_ADDRESS = 0x0900A3C8
CAVE = bytes.fromhex(
    "70b51199ca88139b002b20d1114c2468114d2d88641b2404240c00258026002a15d0964202d3b61a0135fae7"
    "002d0ed00026ac4202d3641b0136fae7022e01d902266c1e84237343c91854430919083170bd0000"
    "0cbc000996bb0009"
)
IMMEDIATE_SEND_ADDRESS = 0x0900A374
IMMEDIATE_SEND_STOCK = bytes.fromhex("00f0a8f9")  # bl 0x0900A6C8
PAYLOAD_SOURCE_ADDRESS = 0x09009E0C
PAYLOAD_SOURCE_STOCK = bytes.fromhex(
    "0d99ca880831"
)  # ldr r1,[sp,#0x34]; ldrh r2,[r1,#6]; adds r1,#8
THUMB_NOP = bytes.fromhex("c046")  # mov r8, r8
PEER_DATA_ADDRESS = 0x0900A14C
PEER_DATA_STOCK = bytes.fromhex("07d0")  # beq 0x0900A15E: data -> acknowledge and drop
PEER_DATA_PATCHED = bytes.fromhex(
    "68d1"
)  # bne 0x0900A220: data -> return 0, the frame goes to the host

# Layout the host needs, as WLAN core addresses.
FRAMES_PER_ENTRY_AREA = 128
KEEPALIVE_ENTRIES = 3


class WlanFirmwarePatchError(Exception):
    """The firmware is not the known stock image, or a record could not be found."""


def thumb_bl(source: int, target: int) -> bytes:
    """A Thumb BL (two halfwords, ARMv5) at source to target."""
    offset = target - (source + 4)
    if not -(1 << 22) <= offset < (1 << 22) or offset & 1:
        raise WlanFirmwarePatchError(f"BL from {source:#x} to {target:#x} out of range")
    first = 0xF000 | ((offset >> 12) & 0x7FF)
    second = 0xF800 | ((offset >> 1) & 0x7FF)
    return struct.pack("<HH", first, second)


def _load_records(image: bytes) -> list[tuple[int, int, int]]:
    """(address, file offset, length) of every load record."""
    if image[:4] != b"XR01":
        raise WlanFirmwarePatchError("not an XR01 WLAN firmware image")
    records = []
    offset = 4
    while offset + 12 <= len(image):
        kind, first, second = struct.unpack_from("<3I", image, offset)
        if kind == 0:
            records.append((first, offset + 12, second))
            offset += 12 + second
        elif kind == 1:
            offset += 12 + 4
        elif kind == 2:
            offset += 8 + first
        elif kind == 4:
            return records
        else:
            raise WlanFirmwarePatchError(f"unknown record {kind:#x} at {offset:#x}")
    raise WlanFirmwarePatchError("no end record")


def _file_offset(records: list[tuple[int, int, int]], address: int, length: int) -> int:
    for start, offset, size in records:
        if start <= address and address + length <= start + size:
            return offset + address - start
    raise WlanFirmwarePatchError(f"{address:#x} is not loaded by the firmware")


def _fix_checksum(image: bytearray) -> None:
    if len(image) % 4:
        raise WlanFirmwarePatchError("image length is not a multiple of 4")
    words = struct.unpack(f"<{len(image) // 4}I", image)
    total = sum(words[:-1]) & 0xFFFFFFFF
    struct.pack_into("<I", image, len(image) - 4, (-total) & 0xFFFFFFFF)


def patch(stock: bytes, verify_hash: bool = True) -> bytes:
    """The patched firmware for the stock image."""
    if verify_hash and hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        raise WlanFirmwarePatchError(
            "wlan_fw.bin is not the known stock firmware; the keepalive patch fits only that version"
        )
    image = bytearray(stock)
    records = _load_records(stock)
    changes = (
        (IMMEDIATE_SEND_ADDRESS, IMMEDIATE_SEND_STOCK, THUMB_NOP * 2),
        (
            PAYLOAD_SOURCE_ADDRESS,
            PAYLOAD_SOURCE_STOCK,
            thumb_bl(PAYLOAD_SOURCE_ADDRESS, CAVE_ADDRESS) + THUMB_NOP,
        ),
        (CAVE_ADDRESS, None, CAVE),
        (PEER_DATA_ADDRESS, PEER_DATA_STOCK, PEER_DATA_PATCHED),
    )
    for address, expected, new in changes:
        offset = _file_offset(records, address, len(new))
        if (
            expected is not None
            and bytes(image[offset : offset + len(expected)]) != expected
        ):
            raise WlanFirmwarePatchError(f"unexpected code at {address:#x}")
        image[offset : offset + len(new)] = new
    _fix_checksum(image)
    return bytes(image)


def write_patched(stock_path: Path, output_path: Path) -> Path:
    """Write the patched firmware unless an identical file is already there."""
    patched = patch(stock_path.read_bytes())
    if not output_path.is_file() or output_path.read_bytes() != patched:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(patched)
    return output_path
