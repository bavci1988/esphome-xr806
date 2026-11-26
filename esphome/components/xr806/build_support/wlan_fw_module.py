"""Builds the XR806 WLAN firmware with the keepalive and sleep modules of esphome-xr806.

wlan_fw.bin is a patch for the ROM of the WLAN core. It is a list of records after the magic "XR01":
``{0, address, length, data}`` loads data, ``{1, entry, 0}`` ends a group and is followed by a 4-byte group header,
``{2, length, data}`` is a register write list and ``{4, address, checksum}`` ends the file. The checksum makes the
sum of all 32-bit words of the file zero.

The modules (wlan_keepalive/keepalive.c and sleep.c) are compiled for the WLAN core and placed over the vendor's
keepalive, its sleep handler and its debug hooks, all inside the data the stock file already loads, so the file keeps
its size. Their data lies in the vendor keepalive's data area, which the file does not load; module_init clears it
when the firmware starts. The firmware is connected to the modules in a few places:

- the receive hook and the MIB pre-handler of the function table: the keepalive;
- the handler the firmware registers for the "nothing keeps the core awake" event, the idle hook and the timer
  start of the function table, and the warm start address of the reset code: the sleep path;
- the vendor keepalive's initialisation function becomes a jump to module_init;
- the debug hooks of the function table go back to the ROM's empty functions, as in firmware 02.138.

Only the firmware with the known SHA-256 is changed; see docs/WLAN_FIRMWARE_NOTES.md in the xr806-sdk repository.
Nothing changed is stored: the stock file comes from the SDK and the result is written to the build directory.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import subprocess

STOCK_SHA256 = "0d08b6c6d0c9bcf3af7b3ebdab55cb623f28feb8392343585255be17c98e59c8"
SOURCE_DIR = Path(__file__).with_name("wlan_keepalive")
SOURCES = ("keepalive.c", "sleep.c")
# Addresses of firmware 02.132 that sleep.c needs.
DEFINES = {
    "PATCH_STATE": "0xFFF0AFD0U",
    "PATCH_BUSY_CHECK": "0xFFF0959DU",
    "PATCH_TIMER_START": "0xFFF09041U",
}
CFLAGS = (
    "-mcpu=arm926ej-s",
    "-mthumb",
    "-Os",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-ffreestanding",
    "-fno-builtin",
    "-nostdlib",
    "-fno-common",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-jump-tables",
    "-fno-tree-loop-distribute-patterns",
)
# Free code areas of 02.132: the vendor keepalive (two words in it are read by other functions and stay), the
# vendor sleep handler and the debug hooks.
CODE_AREAS = (
    (0x09009D84, 0x0900A084),
    (0x0900A088, 0x0900A5B8),
    (0x0900A5DC, 0x0900A800),
    (0x0900A804, 0x0900A820),
    (0x0900B6DC, 0x0900B830),
    (0x0900B408, 0x0900B4F4),
)
# The vendor keepalive's data. The host reads and writes the modules' data here, so the addresses are fixed.
DATA_AREA = (0x0900BB8C, 0x0900BFB0)
KEEPALIVE_ADDRESS = 0x0900BB8C
SLEEP_RECORD_ADDRESS = 0x0900BEA8
DATA_ORDER = (".bss.ka", ".bss.sleep_record")

THUMB = 1
INIT_ADDRESS = 0x0900076C
INIT_STOCK = bytes.fromhex("70b50b490b480022")
INIT_JUMP = bytes.fromhex("004b1847")  # ldr r3, [pc]; bx r3; then the address


@dataclass(frozen=True)
class Hook:
    """A word of the firmware that is pointed at a function of the modules, or at a ROM address."""

    address: int
    stock: int
    target: str | int


HOOKS = (
    Hook(0xFFF0072C, 0x0900A013, "keepalive_receive"),
    Hook(0xFFF00730, 0x0900A227, "keepalive_mib"),
    Hook(0x09001388, 0x0900B6DD, "sleep_event"),
    Hook(0xFFF00438, 0x00019223, "sleep_idle"),
    Hook(0xFFF00468, 0xFFF09041, "sleep_timer_start"),
    Hook(0xFFF000D4, 0x000054E7, "sleep_warm_start"),
    # debug hooks: the ROM's empty functions
    Hook(0xFFF0041C, 0x0900B409, 0x00019215),
    Hook(0xFFF00420, 0x0900B40F, 0x00019217),
    Hook(0xFFF00424, 0x0900B41D, 0x00019219),
    Hook(0xFFF00428, 0x0900B42F, 0x0001921B),
    Hook(0xFFF0042C, 0x0900B471, 0x0001921D),
    Hook(0xFFF00430, 0x0900B4C1, 0x0001921F),
    Hook(0xFFF00434, 0x0900B4DD, 0x00019221),
)


class WlanFirmwareModuleError(Exception):
    """The firmware is not the known stock image, or the modules could not be built or placed."""


def pack(sizes: dict[str, int], areas: tuple[tuple[int, int], ...]) -> list[list[str]]:
    """Distributes the sections over the areas, largest first into the first area with room (4-byte alignment)."""
    room = [end - start for start, end in areas]
    placed: list[list[str]] = [[] for _ in areas]
    for name in sorted(sizes, key=lambda n: (-sizes[n], n)):
        need = (sizes[name] + 3) & ~3
        for index, free in enumerate(room):
            if need <= free:
                room[index] -= need
                placed[index].append(name)
                break
        else:
            raise WlanFirmwareModuleError(
                f"no room for {name} ({sizes[name]} bytes) in the WLAN firmware"
            )
    return placed


def linker_script(placed: list[list[str]]) -> str:
    lines = ["SECTIONS", "{"]
    for index, (names, (start, end)) in enumerate(zip(placed, CODE_AREAS, strict=True)):
        if not names:
            continue
        inputs = " ".join(f"*({name}) . = ALIGN(4);" for name in names)
        lines.append(f"  .code{index} 0x{start:08X} : {{ {inputs} }}")
        lines.append(
            f'  ASSERT(. <= 0x{end:08X}, "code area {index} of the WLAN firmware is full")'
        )
    data = " ".join(f"*({name})" for name in DATA_ORDER)
    lines.append(f"  .bss 0x{DATA_AREA[0]:08X} (NOLOAD) : {{ {data} *(.bss*) }}")
    lines.append(
        f'  ASSERT(. <= 0x{DATA_AREA[1]:08X}, "data area of the WLAN firmware is full")'
    )
    lines.append("  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) }")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise WlanFirmwareModuleError(
            f"{Path(command[0]).name} failed: {result.stderr.strip()}"
        )
    return result.stdout


@dataclass(frozen=True)
class Modules:
    """The linked modules: the code of each area and the addresses of the symbols."""

    code: dict[int, bytes]
    symbols: dict[str, int]


def build_modules(toolchain_bin: Path, work_dir: Path) -> Modules:
    """Compiles and links the modules with the ARM toolchain for the areas of the firmware."""
    work_dir.mkdir(parents=True, exist_ok=True)
    prefix = str(toolchain_bin / "arm-none-eabi-")
    objects = []
    sizes: dict[str, int] = {}
    for source in SOURCES:
        obj = work_dir / (Path(source).stem + ".o")
        _run(
            [prefix + "gcc", *CFLAGS, *(f"-D{k}={v}" for k, v in DEFINES.items()), "-c"]
            + [str(SOURCE_DIR / source), "-o", str(obj)]
        )
        objects.append(str(obj))
        for line in _run([prefix + "size", "-A", str(obj)]).splitlines():
            fields = line.split()
            if len(fields) < 2 or not fields[1].isdigit() or int(fields[1]) == 0:
                continue
            if fields[0].startswith((".text", ".rodata")):
                sizes[fields[0]] = int(fields[1])
            elif fields[0].startswith(".data"):
                raise WlanFirmwareModuleError(
                    f"{source}: {fields[0]} would need loaded data; the modules' data is cleared at start"
                )
    script = work_dir / "modules.ld"
    script.write_text(linker_script(pack(sizes, CODE_AREAS)))
    elf = work_dir / "modules.elf"
    _run([prefix + "ld", "-T", str(script), *objects, "-o", str(elf)])
    symbols = {}
    for line in _run([prefix + "nm", str(elf)]).splitlines():
        value, _, name = line.split()
        symbols[name] = int(value, 16)
    code = {}
    for index, (start, end) in enumerate(CODE_AREAS):
        binary = work_dir / f"code{index}.bin"
        binary.unlink(missing_ok=True)
        _run(
            [prefix + "objcopy", "-O", "binary", "-j", f".code{index}", str(elf)]
            + [str(binary)]
        )
        data = binary.read_bytes() if binary.is_file() else b""
        if len(data) > end - start:
            raise WlanFirmwareModuleError(f"code area {index} overflows")
        if data:
            code[start] = data
    if (
        symbols.get("ka") != KEEPALIVE_ADDRESS
        or symbols.get("sleep_record") != SLEEP_RECORD_ADDRESS
    ):
        raise WlanFirmwareModuleError(
            "the modules' data moved; the host reads it at fixed addresses"
        )
    return Modules(code, symbols)


def _load_records(image: bytes) -> list[tuple[int, int, int]]:
    """(address, file offset, length) of every load record."""
    if image[:4] != b"XR01":
        raise WlanFirmwareModuleError("not an XR01 WLAN firmware image")
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
            raise WlanFirmwareModuleError(f"unknown record {kind:#x} at {offset:#x}")
    raise WlanFirmwareModuleError("no end record")


def _file_offset(records: list[tuple[int, int, int]], address: int, length: int) -> int:
    for start, offset, size in records:
        if start <= address and address + length <= start + size:
            return offset + address - start
    raise WlanFirmwareModuleError(f"{address:#x} is not loaded by the firmware")


def install(stock: bytes, modules: Modules, verify_hash: bool = True) -> bytes:
    """The firmware with the modules in place of the vendor code and connected to it."""
    if verify_hash and hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        raise WlanFirmwareModuleError(
            "wlan_fw.bin is not the known stock firmware; the modules fit only that version"
        )
    image = bytearray(stock)
    records = _load_records(stock)

    def write(address: int, data: bytes, expected: bytes | None = None) -> None:
        offset = _file_offset(records, address, len(data))
        if (
            expected is not None
            and bytes(image[offset : offset + len(expected)]) != expected
        ):
            raise WlanFirmwareModuleError(f"unexpected content at {address:#x}")
        image[offset : offset + len(data)] = data

    def function(name: str) -> int:
        if name not in modules.symbols:
            raise WlanFirmwareModuleError(f"the modules have no {name}")
        return modules.symbols[name] | THUMB

    for address, data in modules.code.items():
        write(address, data)
    for hook in HOOKS:
        target = function(hook.target) if isinstance(hook.target, str) else hook.target
        write(hook.address, struct.pack("<I", target), struct.pack("<I", hook.stock))
    write(
        INIT_ADDRESS,
        INIT_JUMP + struct.pack("<I", function("module_init")),
        INIT_STOCK,
    )

    words = struct.unpack(f"<{len(image) // 4}I", image)
    struct.pack_into("<I", image, len(image) - 4, (-sum(words[:-1])) & 0xFFFFFFFF)
    return bytes(image)


def write_firmware(stock_path: Path, output_path: Path, toolchain_bin: Path) -> Path:
    """Builds the firmware at build time and writes it unless an identical file is already there."""
    modules = build_modules(toolchain_bin, output_path.parent / "wlan_fw_modules")
    firmware = install(stock_path.read_bytes(), modules)
    if not output_path.is_file() or output_path.read_bytes() != firmware:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(firmware)
    return output_path
