"""Tests for the tool that builds the XR806 WLAN firmware with the keepalive and sleep modules."""

from pathlib import Path
import shutil
import struct

import pytest

from esphome.components.xr806.build_support import wlan_fw_module as module

AREAS = ((0x1000, 0x1040), (0x2000, 0x2010))


def _image() -> bytes:
    """A firmware image whose load records hold the stock words at the places the tool changes."""
    lows = bytearray(0x1000)
    code = bytearray(0xC000 - 0x400)

    def put(address: int, data: bytes) -> None:
        if address >= 0xFFF00000:
            lows[address - 0xFFF00000 : address - 0xFFF00000 + len(data)] = data
        else:
            code[address - 0x09000400 : address - 0x09000400 + len(data)] = data

    for hook in module.HOOKS:
        put(hook.address, struct.pack("<I", hook.stock))
    put(module.INIT_ADDRESS, module.INIT_STOCK)
    image = bytearray(b"XR01")
    image += struct.pack("<3I", 0, 0xFFF00000, len(lows)) + lows
    image += struct.pack("<3I", 0, 0x09000400, len(code)) + code
    image += struct.pack("<3I", 1, 0, 0) + struct.pack("<I", 0)
    image += struct.pack("<3I", 4, 0, 0)
    words = struct.unpack(f"<{len(image) // 4}I", image)
    struct.pack_into("<I", image, len(image) - 4, (-sum(words[:-1])) & 0xFFFFFFFF)
    return bytes(image)


def _word(image: bytes, address: int) -> int:
    lows_offset = 4 + 12
    code_offset = lows_offset + 0x1000 + 12
    if address >= 0xFFF00000:
        return struct.unpack_from("<I", image, lows_offset + address - 0xFFF00000)[0]
    return struct.unpack_from("<I", image, code_offset + address - 0x09000400)[0]


def _modules() -> module.Modules:
    symbols = {
        hook.target: 0x0900A000 + 0x10 * index
        for index, hook in enumerate(module.HOOKS)
        if isinstance(hook.target, str)
    }
    symbols["module_init"] = 0x0900B408
    return module.Modules({module.CODE_AREAS[0][0]: b"\x11\x22\x33\x44"}, symbols)


def test_pack_puts_the_largest_first() -> None:
    # 0x30 and 0x10 fill the first area; 0x0E goes to the second, where the small one still fits
    assert module.pack({"a": 0x30, "b": 0x10, "c": 0x0C, "d": 2}, AREAS) == [
        ["a", "b"],
        ["c", "d"],
    ]


def test_pack_rounds_up_to_words() -> None:
    assert module.pack({"a": 0x3D}, AREAS) == [["a"], []]
    assert module.pack({"a": 0x3D, "b": 1}, AREAS) == [["a"], ["b"]]


def test_pack_reports_a_section_that_does_not_fit() -> None:
    with pytest.raises(module.WlanFirmwareModuleError, match="no room for big"):
        module.pack({"big": 0x44}, AREAS)


def test_linker_script_keeps_the_data_out_of_the_image() -> None:
    script = module.linker_script([[".text.a"], [], [], [], [], []])

    assert ".code0 0x09009D84 : { *(.text.a) . = ALIGN(4); }" in script
    assert ".code1" not in script
    assert (
        ".bss 0x0900BB8C (NOLOAD) : { *(.bss.ka) *(.bss.sleep_record) *(.bss*) }"
        in script
    )


def test_install_connects_the_firmware_and_keeps_the_checksum() -> None:
    stock = _image()
    firmware = module.install(stock, _modules(), verify_hash=False)

    assert len(firmware) == len(stock)
    assert sum(struct.unpack(f"<{len(firmware) // 4}I", firmware)) & 0xFFFFFFFF == 0
    assert _word(firmware, module.CODE_AREAS[0][0]) == 0x44332211
    # functions of the modules get the Thumb bit, ROM addresses are written as they are
    assert _word(firmware, 0xFFF0072C) == 0x0900A001
    assert _word(firmware, 0xFFF0041C) == 0x00019215
    # ldr r3, [pc]; bx r3; module_init
    assert _word(firmware, module.INIT_ADDRESS) == 0x47184B00
    assert _word(firmware, module.INIT_ADDRESS + 4) == 0x0900B409


def test_install_refuses_other_content() -> None:
    stock = bytearray(_image())
    offset = 4 + 12 + 0x72C
    stock[offset] ^= 0xFF
    with pytest.raises(module.WlanFirmwareModuleError, match="unexpected content"):
        module.install(bytes(stock), _modules(), verify_hash=False)


def test_install_refuses_an_unknown_firmware() -> None:
    with pytest.raises(module.WlanFirmwareModuleError, match="not the known stock"):
        module.install(_image(), _modules())


def test_install_reports_a_missing_function() -> None:
    modules = module.Modules({}, {"keepalive_receive": 0x0900A000})
    with pytest.raises(module.WlanFirmwareModuleError, match="have no"):
        module.install(_image(), modules, verify_hash=False)


@pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None, reason="needs the ARM toolchain"
)
def test_modules_build_and_fit(tmp_path: Path) -> None:
    toolchain = Path(shutil.which("arm-none-eabi-gcc")).parent
    modules = module.build_modules(toolchain, tmp_path)

    assert modules.symbols["ka"] == module.KEEPALIVE_ADDRESS
    assert modules.symbols["sleep_record"] == module.SLEEP_RECORD_ADDRESS
    for hook in module.HOOKS:
        if isinstance(hook.target, str):
            assert hook.target in modules.symbols
    for start, code in modules.code.items():
        end = next(e for s, e in module.CODE_AREAS if s == start)
        assert start + len(code) <= end
