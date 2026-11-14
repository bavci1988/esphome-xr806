"""Tests for the XR806 xradio toolchain downloads."""

from __future__ import annotations

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
