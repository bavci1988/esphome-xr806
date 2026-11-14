"""Tests for the XR806 xradio toolchain downloads."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.build_helpers.native import NATIVE_TOOLCHAIN_MODULES
from esphome.build_helpers.tools_cache import TOOLS_CACHE_SPECS, XRADIO_TOOLS_CACHE
from esphome.components.xr806 import framework
from esphome.const import NATIVE_TOOLCHAINS, Toolchain
from esphome.core import EsphomeError


def test_xradio_is_a_registered_native_toolchain() -> None:
    assert Toolchain.XRADIO in NATIVE_TOOLCHAINS
    assert (
        NATIVE_TOOLCHAIN_MODULES[("xr806", Toolchain.XRADIO)]
        == "esphome.components.xr806.toolchain"
    )
    assert XRADIO_TOOLS_CACHE in TOOLS_CACHE_SPECS


def test_sdk_download_is_the_pinned_release() -> None:
    tag, sha256, size = framework.SDK_RELEASE
    download = framework.sdk_download()

    assert download.url == (
        f"https://github.com/bavci1988/xr806-sdk/releases/download/{tag}/"
        f"xr806-sdk-{tag}.tar.gz"
    )
    assert download.sha256 == sha256
    assert download.size == size


@pytest.mark.parametrize("systype", sorted(framework.TOOLCHAIN_BUILDS))
def test_toolchain_download_per_host(
    monkeypatch: pytest.MonkeyPatch, systype: str
) -> None:
    monkeypatch.setattr(framework, "get_systype", lambda: systype)
    suffix, sha256, size = framework.TOOLCHAIN_BUILDS[systype]
    download = framework.toolchain_download()

    assert download.url.endswith(
        f"/v{framework.TOOLCHAIN_VERSION}/xpack-arm-none-eabi-gcc-"
        f"{framework.TOOLCHAIN_VERSION}-{suffix}"
    )
    assert download.sha256 == sha256
    assert download.size == size


def test_toolchain_download_unknown_host(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(framework, "get_systype", lambda: "linux_riscv64")

    with pytest.raises(EsphomeError, match="no arm-none-eabi toolchain"):
        framework.toolchain_download()


def test_build_env_puts_the_toolchain_first(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv("PATH", "/usr/bin")
    monkeypatch.setenv("CC_DIR", "/opt/other")
    env = framework.get_build_env(tmp_path)

    assert env["PATH"].split(":")[0] == str(tmp_path)
    assert "CC_DIR" not in env


def test_library_fragment(tmp_path: Path) -> None:
    from esphome.arduino.library import ArduinoLibrary
    from esphome.components.xr806.build_support.libraries import write_fragment

    src = tmp_path / "src"
    sodium = ArduinoLibrary(
        name="esphome/libsodium",
        sources=[src / "core.c", src / "util.cpp"],
        include_dirs=[src / "include"],
        flags=["-DCONFIGURED=1"],
    )
    json = ArduinoLibrary(name="bblanchon/ArduinoJson", include_dirs=[src / "json"])
    fragment = tmp_path / "libraries.mk"
    write_fragment([json, sodium], tmp_path / "obj", fragment)
    text = fragment.read_text(encoding="utf-8")

    assert f"XR806_LIBRARY_INCLUDES += -I{src / 'json'}" in text
    assert f"XR806_LIBRARY_INCLUDES += -I{src / 'include'}" in text
    # A C source builds with the C compiler and the library's own flags
    assert (
        "$(CC) $(XR806_LIBRARY_CFLAGS) $(XR806_LIBRARY_INCLUDES) -DCONFIGURED=1" in text
    )
    assert "$(XR806_CXX) $(XR806_CXXFLAGS) -DCONFIGURED=1" in text
    # Only the library with sources gets an archive
    assert text.count("XR806_LIBRARY_ARCHIVES += ") == 1
    assert "1_esphome_libsodium/lib1.a" in text


def test_api_encryption_adds_noise_libraries(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    from esphome.core import CORE

    generate_main(component_config_path("api_encryption.yaml"))

    names = {library.name for library in CORE.platformio_libraries.values()}
    assert {"esphome/noise-c", "esphome/libsodium"} <= names
