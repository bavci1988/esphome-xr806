"""Tests for the TRV16 thermostat component."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def test_tuya_trv16_generates(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_config_path("trv16.yaml"))

    assert "tuya_trv16::TuyaTRV16(" in main_cpp
    assert "set_force_manual_mode(true)" in main_cpp
    assert "set_awaiting_confirmation_binary_sensor(" in main_cpp
