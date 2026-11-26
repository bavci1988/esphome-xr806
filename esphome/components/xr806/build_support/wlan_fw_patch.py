"""Patch of the XR806 WLAN firmware (wlan_fw.bin) for the keepalive frame list.

wlan_fw.bin is a patch for the ROM of the WLAN core. It is a list of records after the magic "XR01":
``{0, address, length, data}`` loads data, ``{1, entry, 0}`` ends a group and is followed by a 4-byte group header,
``{2, length, data}`` is a register write list and ``{4, address, checksum}`` ends the file. The checksum makes the
sum of all 32-bit words of the file zero.

The keepalive ("P2P server", MIBs 0x1076..0x1079) is implemented in the patch. This module changes it so that
server 0 sends a different, pre-encrypted frame each time Home Assistant acknowledged the previous one:

1. MIB 0x1078 no longer sends a frame right away (BL at 0x0900A374), so the first frame goes out one period after
   configuration instead of while the host suspends the WLAN.
2. Before the payload copy at 0x09009E0C a new function chooses frame k = (sequence - TcpSeqInit) / length from the
   payload areas of the three keepalive entries and the three wake pattern entries (128 // length frames each).
   The firmware adds the length to the sequence for each acknowledged frame; the IP ID also counts
   acknowledgements and is no frame index. It is placed at 0x0900A3C8, in the UDP keepalive (0x0900A3C6), which is
   only used for servers with the UDP flag; ESPHome uses TCP only.
3. The receive filter (0x0900A012) acknowledged data from the peer itself and dropped it, so a command from Home
   Assistant never reached the host. Data segments are now passed to the host, which wakes it (0x0900A140);
   segments without data are still handled by the firmware. A data segment that also acknowledges the frame in
   flight is only used as that acknowledgement and dropped: passed on, it stalled the WLAN core. The peer sends
   the data again and that copy wakes the host.
   The stock firmware took any segment with ACK set for the acknowledgement of the frame in flight, so a duplicate
   ACK or a crossing ping counted a frame that was lost on the air, and that frame was never sent again. A frame
   now only counts when the acknowledgement number is its sequence plus its length. A RST or FIN from the peer is
   passed to the host, so that it learns at once that Home Assistant closed the connection.
4. The frame chooser also ends the keepalive after W frames, W being the byte at 0x0900BC73 (reserved1 of the
   keepalive configuration; 0 = off), which it counts down for every frame built, retries included: with frame W it
   sets the keepalive period (0x0900BC70) to 255 s. Keepalive entry 0 stays enabled, because the receive filter only
   passes data from the peer to the host while it is; with host sleep set it drops that data otherwise. Home
   Assistant then pings after its own idle time and that data wakes the host (change 3).
5. While the host sleeps the receive filter drops every IPv4 packet that is not from the keepalive peer, so the web
   server and OTA could not wake the host. A TCP SYN (no other flag, not a fragment) to the web server or OTA port
   or the native API port is now passed to the host (0x0900A216), so that Home Assistant can also connect again
   after it closed the connection. Nothing else is let through.

Only the firmware with the known SHA-256 is patched; see docs/WLAN_FIRMWARE_NOTES.md in the xr806-sdk repository.
Nothing patched is stored: the stock file comes from the SDK and the patch is applied in the build directory.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct
import subprocess

STOCK_SHA256 = "0d08b6c6d0c9bcf3af7b3ebdab55cb623f28feb8392343585255be17c98e59c8"

# Thumb (ARMv5TE), assembled from wlan_fw_keepalive.S at build time. Both areas are in the UDP keepalive
# (0x0900A3C6..0x0900A5B8), which is only used for servers with the UDP flag; ESPHome uses TCP only. Its first
# part ends at the literal pool of the MIB handler (0x0900A4A0..0x0900A4C8), which stays.
CAVE_SOURCE = Path(__file__).with_name("wlan_fw_keepalive.S")
CAVE_ADDRESS = 0x0900A3C8
CAVE_MAX_SIZE = 0x0900A4A0 - CAVE_ADDRESS
CAVE2_ADDRESS = 0x0900A4C8
CAVE2_MAX_SIZE = 0x0900A5B8 - CAVE2_ADDRESS
# wlan_fw_keepalive.S starts with three branches: the frame chooser at CAVE_ADDRESS, the receive filter decision at
# +2 and the handling of a segment from the peer at +4.
CAVE_FILTER_ENTRY = CAVE_ADDRESS + 2
CAVE_PEER_ENTRY = CAVE_ADDRESS + 4
IMMEDIATE_SEND_ADDRESS = 0x0900A374
IMMEDIATE_SEND_STOCK = bytes.fromhex("00f0a8f9")  # bl 0x0900A6C8
PAYLOAD_SOURCE_ADDRESS = 0x09009E0C
PAYLOAD_SOURCE_STOCK = bytes.fromhex(
    "0d99ca880831"
)  # ldr r1,[sp,#0x34]; ldrh r2,[r1,#6]; adds r1,#8
THUMB_NOP = bytes.fromhex("c046")  # mov r8, r8
# The acknowledgement handling of the receive filter: "adds r1,r0,r1; movs r0,r4; bl 0x0900A720; ldr r1,[sp,#8];
# cmp r1,#0; beq 0x0900A15E", then the firmware acknowledges data itself and drops it. Replaced as a whole.
PEER_DATA_ADDRESS = 0x0900A140
PEER_DATA_STOCK = bytes.fromhex(
    "4118200000f0ecfa0299002907d000220092029a079804992300fff744ff"
)
PEER_DATA_END = (
    0x0900A15E  # movs r0,#1; str r0,[sp,#0xc]: the frame is used up, it is dropped
)
PEER_DATA_TO_HOST = 0x0900A220  # return [sp,#0x18], 0 here: the frame goes to the host
FILTER_DROP_ADDRESS = 0x0900A216
# ldr r0,[sp,#0x14]; cmp r0,#0; bne 0x0900A220; movs r0,#1 (then str r0,[sp,#0x18])
FILTER_DROP_STOCK = bytes.fromhex("0598002801d10120")
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


def _thumb(*halfwords: int) -> bytes:
    return b"".join(struct.pack("<H", h) for h in halfwords)


def peer_data_code() -> bytes:
    """The replacement for the acknowledgement handling at PEER_DATA_ADDRESS.

    The cave decides (see peer_segment in wlan_fw_keepalive.S); r0 = 0 means the firmware used the segment up.
    """
    bl_address = PEER_DATA_ADDRESS
    beq_address = PEER_DATA_ADDRESS + 6
    b_address = PEER_DATA_ADDRESS + 8
    code = thumb_bl(bl_address, CAVE_PEER_ENTRY) + _thumb(
        0x2800,  # cmp r0,#0
        0xD000 | ((PEER_DATA_END - beq_address - 4) // 2),  # beq: used up, dropped
        0xE000 | ((PEER_DATA_TO_HOST - b_address - 4) // 2),  # b: to the host
    )
    code += THUMB_NOP * ((len(PEER_DATA_STOCK) - len(code)) // 2)
    if len(code) != len(PEER_DATA_STOCK):
        raise WlanFirmwarePatchError("peer data code has the wrong size")
    return code


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


def assemble_cave(toolchain_bin: Path, work_dir: Path) -> tuple[bytes, bytes]:
    """Assemble wlan_fw_keepalive.S with the ARM toolchain; returns the code at CAVE_ADDRESS and CAVE2_ADDRESS."""
    work_dir.mkdir(parents=True, exist_ok=True)
    obj = work_dir / "wlan_fw_keepalive.o"
    elf = work_dir / "wlan_fw_keepalive.elf"
    binary = work_dir / "wlan_fw_keepalive.bin"
    binary2 = work_dir / "wlan_fw_keepalive2.bin"
    prefix = str(toolchain_bin / "arm-none-eabi-")
    steps = (
        [
            prefix + "as",
            "-mcpu=arm926ej-s",
            "-mthumb",
            str(CAVE_SOURCE),
            "-o",
            str(obj),
        ],
        [
            prefix + "ld",
            f"-Ttext=0x{CAVE_ADDRESS:08X}",
            f"--section-start=.cave2=0x{CAVE2_ADDRESS:08X}",
            "-e",
            "cave",
            str(obj),
            "-o",
            str(elf),
        ],
        [prefix + "objcopy", "-O", "binary", "-j", ".text", str(elf), str(binary)],
        [prefix + "objcopy", "-O", "binary", "-j", ".cave2", str(elf), str(binary2)],
    )
    for step in steps:
        result = subprocess.run(step, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise WlanFirmwarePatchError(
                f"{Path(step[0]).name} failed: {result.stderr.strip()}"
            )
    code = binary.read_bytes()
    code2 = binary2.read_bytes()
    if not code or len(code) > CAVE_MAX_SIZE:
        raise WlanFirmwarePatchError(
            f"frame chooser is {len(code)} bytes, at most {CAVE_MAX_SIZE} fit"
        )
    if not code2 or len(code2) > CAVE2_MAX_SIZE:
        raise WlanFirmwarePatchError(
            f"receive filter code is {len(code2)} bytes, at most {CAVE2_MAX_SIZE} fit"
        )
    return code, code2


def patch(stock: bytes, cave: tuple[bytes, bytes], verify_hash: bool = True) -> bytes:
    """The patched firmware for the stock image, with the assembled frame chooser."""
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
        (
            FILTER_DROP_ADDRESS,
            FILTER_DROP_STOCK,
            thumb_bl(FILTER_DROP_ADDRESS, CAVE_FILTER_ENTRY) + THUMB_NOP * 2,
        ),
        (CAVE_ADDRESS, None, cave[0]),
        (CAVE2_ADDRESS, None, cave[1]),
        (PEER_DATA_ADDRESS, PEER_DATA_STOCK, peer_data_code()),
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


def write_patched(stock_path: Path, output_path: Path, toolchain_bin: Path) -> Path:
    """Patch the stock firmware at build time and write it unless an identical file is already there."""
    cave = assemble_cave(toolchain_bin, output_path.parent / "wlan_fw_keepalive")
    patched = patch(stock_path.read_bytes(), cave)
    if not output_path.is_file() or output_path.read_bytes() != patched:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(patched)
    return output_path
