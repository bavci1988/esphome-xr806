"""Tests for the Tuya low_power protocol configuration on XR806."""

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
        "low_power.yaml",
        "low_power_network_suspend.yaml",
        "low_power_ota.yaml",
        "low_power_wake_pin.yaml",
    ],
)
def test_low_power_generates(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
    fixture: str,
) -> None:
    main_cpp = generate_main(component_config_path(fixture))

    assert "USE_TUYA_LOW_POWER" in defines()
    assert "xr806::XR806StandbyController" in main_cpp
    assert "xr806::XR806TuyaLowPowerHost(" in main_cpp


def test_on_mcu_ready(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_config_path("low_power.yaml"))

    assert "add_on_mcu_ready_callback(" in main_cpp


@pytest.mark.parametrize(
    ("fixture", "message"),
    [
        ("low_power_pb04.yaml", "[trv16_mcu_wake_pb04] is an invalid option"),
        ("low_power_network_suspend_no_standby.yaml", "requires xr806.standby"),
        (
            "refresh_standard_protocol.yaml",
            "tuya.refresh requires protocol: low_power",
        ),
        (
            "datapoint_lists_standard_protocol.yaml",
            "status_datapoints requires protocol: low_power",
        ),
    ],
)
def test_low_power_rejected(
    component_config_path: Callable[[str], Path],
    capsys: pytest.CaptureFixture[str],
    fixture: str,
    message: str,
) -> None:
    CORE.config_path = component_config_path(fixture)

    assert read_config({}) is None
    assert message in capsys.readouterr().out


def test_low_power_wake_pin_pb4(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    # The original WXU firmware drives PB4 (WXU pad 2) as an active-low MCU wake output.
    main_cpp = generate_main(component_config_path("low_power_wake_pin.yaml"))

    assert "set_mcu_wake_pin(" in main_cpp
    assert "set_pin(36)" in main_cpp
    assert "set_inverted(true)" in main_cpp


def test_low_power_datapoint_lists(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    # A version-0 MCU other than the TRV16 names its own status and refresh datapoints.
    main_cpp = generate_main(component_config_path("low_power_datapoints.yaml"))

    assert (
        "set_low_power_status_datapoints({2, 3, 8, 16, 19, 24, 25, 26, 27, 35, 40, 45, "
        "101, 102, 104, 106})" in main_cpp
    )
    assert "set_low_power_refresh_datapoints({2, 3, 8, 16, 19, 24})" in main_cpp
    assert "set_low_power_refresh_trigger_datapoint(106)" in main_cpp
