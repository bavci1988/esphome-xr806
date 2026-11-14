"""Tests for the Tuya low_power_v0 protocol configuration on XR806."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.config import read_config
from esphome.core import CORE


def defines() -> set[str]:
    return {define.name for define in CORE.defines}


@pytest.mark.parametrize(
    "fixture",
    [
        "low_power_v0.yaml",
        "low_power_v0_network_suspend.yaml",
        "low_power_v0_ota.yaml",
        "low_power_v0_wake_pin.yaml",
    ],
)
def test_low_power_v0_generates(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
    fixture: str,
) -> None:
    main_cpp = generate_main(component_config_path(fixture))

    assert "USE_TUYA_LOW_POWER_V0" in defines()
    assert "xr806::XR806StandbyController" in main_cpp
    assert "xr806::XR806TuyaLowPowerHost(" in main_cpp


def test_low_power_v0_product_id(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_config_path("low_power_v0.yaml"))

    assert 'set_product_id("ihedujbuli6hsx9b")' in main_cpp


def test_on_mcu_ready(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_config_path("low_power_v0.yaml"))

    assert "add_on_mcu_ready_callback(" in main_cpp


@pytest.mark.parametrize(
    ("fixture", "message"),
    [
        ("low_power_v0_pb04.yaml", "[trv16_mcu_wake_pb04] is an invalid option"),
        ("low_power_v0_network_suspend_no_standby.yaml", "requires xr806.standby"),
        (
            "refresh_standard_protocol.yaml",
            "tuya.refresh requires protocol: low_power",
        ),
    ],
)
def test_low_power_v0_rejected(
    component_config_path: Callable[[str], Path],
    capsys: pytest.CaptureFixture[str],
    fixture: str,
    message: str,
) -> None:
    CORE.config_path = component_config_path(fixture)

    assert read_config({}) is None
    assert message in capsys.readouterr().out


def test_low_power_v0_wake_pin_pb4(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    # The original WXU firmware drives PB4 (WXU pad 2) as an active-low MCU wake output.
    main_cpp = generate_main(component_config_path("low_power_v0_wake_pin.yaml"))

    assert "set_mcu_wake_pin(" in main_cpp
    assert "set_pin(36)" in main_cpp
    assert "set_inverted(true)" in main_cpp
