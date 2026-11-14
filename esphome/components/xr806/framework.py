"""Download and install the XR806 SDK and the arm-none-eabi gcc for the xradio toolchain.

Artifacts land in a machine-global cache shared across projects:

    <cache>/xradio/frameworks/<tag>/        the SDK
    <cache>/xradio/toolchains/<version>/    arm-none-eabi gcc

Both are pinned release archives with their SHA-256. ``ESPHOME_XRADIO_SDK_MIRRORS``
and ``ESPHOME_XRADIO_TOOLCHAIN_MIRRORS`` override the URLs, with ``{VERSION}``
standing for the release tag and ``{SYSTEM}`` for the host. For SDK development,
``XR806_SDK_SOURCE_DIR`` points the build at a local SDK checkout instead.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import NamedTuple

from esphome.build_helpers.tools_cache import XRADIO_TOOLS_CACHE, tools_cache_path
from esphome.core import EsphomeError
from esphome.framework_helpers import str_to_lst_of_str
from esphome.platformio.registry import (
    Download,
    PackageSpec,
    Resolver,
    get_systype,
    install_packages,
    prefetch_packages,
)

SDK_PACKAGE = "xr806-sdk"
_SDK_RELEASES = "https://github.com/bavci1988/xr806-sdk/releases/"
# Release tag of the SDK source archive, its SHA-256 and size in bytes
SDK_RELEASE = (
    "1.2.2-esphome.1",
    "c3dcf2fb24b1df4ca7853379164bc9ee15a03ebb8a0f9ea8c03b4be574842ec0",
    75618691,
)
# Directories every SDK build needs; checked after extraction
SDK_LAYOUT = (
    "project/demo/trv16_port/gcc",
    "project/common/board/xr806_wxu",
    "lib/xradio_v3",
    "bin/xradio_v3",
    "include",
)

TOOLCHAIN_PACKAGE = "xpack-arm-none-eabi-gcc"
# gcc 13.2, the compiler the SDK and the ESPHome XR806 sources are built and tested with
TOOLCHAIN_VERSION = "13.2.1-1.1"
_TOOLCHAIN_RELEASES = (
    "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/"
)
# Registry system tag -> (archive suffix, sha256, size) of that host's archive
TOOLCHAIN_BUILDS: dict[str, tuple[str, str, int]] = {
    "darwin_arm64": (
        "darwin-arm64.tar.gz",
        "d4ce0de062420daab140161086ba017642365977e148d20f55a8807b1eacd703",
        258277148,
    ),
    "darwin_x86_64": (
        "darwin-x64.tar.gz",
        "1ecc0fd6c31020aff702204f51459b4b00ff0d12b9cd95e832399881d819aa57",
        262217286,
    ),
    "linux_aarch64": (
        "linux-arm64.tar.gz",
        "ab7f75d95ead0b1efb7432e7f034f9575cc3d23dc1b03d41af1ec253486d19de",
        265190134,
    ),
    "linux_x86_64": (
        "linux-x64.tar.gz",
        "1252a8cafe9237de27a765376697230368eec21db44dc3f1edeb8d838dabd530",
        268158135,
    ),
    "windows_amd64": (
        "win32-x64.zip",
        "56b18ccb0a50f536332ec5de57799342ff0cd005ca2c54288c74759b51929e4f",
        306340878,
    ),
}
_TOOLCHAIN_DIR = f"{TOOLCHAIN_PACKAGE}-{TOOLCHAIN_VERSION}"

ESPHOME_XRADIO_SDK_MIRRORS = str_to_lst_of_str(
    os.environ.get("ESPHOME_XRADIO_SDK_MIRRORS", "")
)
ESPHOME_XRADIO_TOOLCHAIN_MIRRORS = str_to_lst_of_str(
    os.environ.get("ESPHOME_XRADIO_TOOLCHAIN_MIRRORS", "")
)


def get_xradio_tools_path() -> Path:
    return tools_cache_path(*XRADIO_TOOLS_CACHE)


def sdk_download() -> Download:
    tag, sha256, size = SDK_RELEASE
    url = f"{_SDK_RELEASES}download/{tag}/{SDK_PACKAGE}-{tag}.tar.gz"
    return Download(url, sha256, size)


def toolchain_download() -> Download:
    """The toolchain archive for the current host."""
    systype = get_systype()
    if (build := TOOLCHAIN_BUILDS.get(systype)) is None:
        raise EsphomeError(
            f"There is no arm-none-eabi toolchain for this system ({systype}); "
            f"supported systems are {', '.join(sorted(TOOLCHAIN_BUILDS))}. "
            "Point ESPHOME_XRADIO_TOOLCHAIN_MIRRORS at a toolchain archive"
        )
    suffix, sha256, size = build
    archive = f"{_TOOLCHAIN_DIR}-{suffix}"
    url = f"{_TOOLCHAIN_RELEASES}download/v{TOOLCHAIN_VERSION}/{archive}"
    return Download(url, sha256, size)


class InstalledPaths(NamedTuple):
    """The SDK to build with, its revision, and the toolchain's bin directory."""

    sdk: Path
    sdk_revision: str
    toolchain_bin: Path


def _get_sdk_path() -> Path:
    return get_xradio_tools_path() / "frameworks" / SDK_RELEASE[0]


def _get_toolchain_path() -> Path:
    return get_xradio_tools_path() / "toolchains" / TOOLCHAIN_VERSION


def check_and_install() -> InstalledPaths:
    """Ensure the SDK and the toolchain are installed; return their paths."""
    from .build_support.dependencies import sdk_source_override

    override = sdk_source_override()
    downloads_dir = get_xradio_tools_path() / "downloads"
    sdk_path = _get_sdk_path()
    toolchain_path = _get_toolchain_path()
    specs = [
        PackageSpec(
            TOOLCHAIN_PACKAGE,
            TOOLCHAIN_VERSION,
            toolchain_path,
            ESPHOME_XRADIO_TOOLCHAIN_MIRRORS,
            ("bin", "arm-none-eabi"),
        )
    ]
    if override is None:
        specs.append(
            PackageSpec(
                SDK_PACKAGE,
                SDK_RELEASE[0],
                sdk_path,
                ESPHOME_XRADIO_SDK_MIRRORS,
                SDK_LAYOUT,
            )
        )
    # Resolved only when a download is needed; a mirror override wins
    resolvers: dict[str, Resolver] = {}
    if not ESPHOME_XRADIO_TOOLCHAIN_MIRRORS:
        resolvers[TOOLCHAIN_PACKAGE] = toolchain_download
    if not ESPHOME_XRADIO_SDK_MIRRORS:
        resolvers[SDK_PACKAGE] = sdk_download
    prefetch_packages(specs, downloads_dir, resolvers)
    install_packages(specs, downloads_dir, resolvers)
    if override is not None:
        sdk, revision = override
    else:
        sdk, revision = sdk_path, SDK_RELEASE[0]
    return InstalledPaths(
        sdk=sdk,
        sdk_revision=revision,
        toolchain_bin=toolchain_path / "bin",
    )


def toolchain_tool(name: str) -> Path:
    """Path to one toolchain tool (gcc, objdump, readelf, ...)."""
    suffix = ".exe" if os.name == "nt" else ""
    return _get_toolchain_path() / "bin" / f"arm-none-eabi-{name}{suffix}"


def get_build_env(toolchain_bin: Path) -> dict[str, str]:
    """The environment for the SDK's Makefiles: the downloaded gcc first on PATH."""
    env = os.environ.copy()
    # Drop empty entries: a trailing separator would make the shell search the current directory
    parts = [str(toolchain_bin), *filter(None, env.get("PATH", "").split(os.pathsep))]
    env["PATH"] = os.pathsep.join(parts)
    # The SDK's gcc.mk would use a CC_DIR from the user's shell instead of PATH
    env.pop("CC_DIR", None)
    return env
