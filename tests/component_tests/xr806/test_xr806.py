"""Tests for the XR806 platform configuration."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from esphome.core import CORE


def defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_ota_diagnostic_does_not_enable_updates(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("ota_diagnostic.yaml"))

    assert "USE_XR806_OTA_RUNTIME_PROBE" in defines()
    assert "USE_OTA" not in defines()


def test_ota_diagnostic_with_wifi_and_tuya(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("ota_diagnostic_wifi.yaml"))

    assert {
        "USE_XR806_OTA_RUNTIME_PROBE",
        "USE_WIFI",
        "USE_TUYA_LOW_POWER_V0",
    } <= defines()
    assert "USE_OTA" not in defines()


def test_ota_platform(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("ota.yaml"))

    assert {"USE_OTA", "USE_WIFI", "USE_XR806"} <= defines()
