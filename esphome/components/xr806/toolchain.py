"""The native build backend of the XR806 xradio toolchain.

The build itself runs from the platform's ``run_compile`` hook; this module
gives the binutils, the linked image and the flash image of that build.
"""

from pathlib import Path

from esphome.core import CORE

from .framework import toolchain_tool


def get_objdump_path() -> Path:
    return toolchain_tool("objdump")


def get_readelf_path() -> Path:
    return toolchain_tool("readelf")


def get_addr2line_path() -> Path:
    return toolchain_tool("addr2line")


def get_elf_path() -> Path:
    """The linked application, as the Make adapter writes it."""
    return Path(CORE.build_path) / "xr806" / "app.elf"


def get_factory_firmware_path() -> Path:
    """The full AWIH image (bootloader and application) for a wired flash."""
    return CORE.firmware_bin


def get_idedata() -> dict | None:
    """The SDK's Makefiles write no compile database, so there is no idedata."""
    return None
