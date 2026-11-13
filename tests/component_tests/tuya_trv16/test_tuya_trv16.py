"""Tests for the TRV16 heating program component."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def test_tuya_trv16_generates(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_config_path("trv16.yaml"))

    assert "tuya_trv16::TuyaTRV16(" in main_cpp
    assert "set_day_select(" in main_cpp
    assert "set_point_time(0, " in main_cpp
    assert "set_point_time(5, " in main_cpp
    assert "set_point_temperature(0, " in main_cpp
    assert "set_day_text_sensor(6, " in main_cpp
    assert "set_awaiting_confirmation_binary_sensor(" in main_cpp
    # The chosen day is resolved when pressed; "all days" writes every day.
    assert "tuya_trv16::TuyaTRV16SaveButton(0)" in main_cpp
    assert "tuya_trv16::TuyaTRV16SaveButton(127)" in main_cpp
    assert "tuya_trv16::TuyaTRV16PowerOnButton()" in main_cpp
