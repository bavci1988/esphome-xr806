"""XR806 safe-mode schema and codegen integration.

These tests are NOT proof of boot recovery, flash integrity or OTA on hardware.
The stock ESPHome safe_mode component uses the XR806 FDCM preferences backend.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import re

import pytest

from esphome.config import read_config
from esphome.core import CORE


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_native_ota_autoloads_safe_mode(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("ota.yaml"))

    assert "safe_mode::SafeModeComponent" in generated
    assert "should_enter_safe_mode(" in generated
    assert {"USE_OTA", "USE_PREFERENCE_KEY_LOOKUP"} <= _defines()
    assert "preferences::IntervalSyncer" in generated
    # There is no verified XR806 bootloader rollback integration.
    assert "USE_OTA_ROLLBACK" not in _defines()


def test_explicit_flash_safe_mode_and_manual_recovery_button(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("safe_mode_explicit.yaml"))

    assert "safe_mode::SafeModeComponent" in generated
    assert re.search(
        r"should_enter_safe_mode\(\s*4\s*,\s*120000\s*,\s*30000\s*,\s*true\s*\)",
        generated,
    )
    assert "restart_in_safe_mode()" in generated
    assert "USE_SAFE_MODE_CALLBACK" in _defines()
    assert "add_on_safe_mode_callback" in generated
    assert "USE_PREFERENCE_KEY_LOOKUP" in _defines()
    assert "preferences::IntervalSyncer" in generated


def test_web_server_ota_also_autoloads_safe_mode(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("web_server_ota_enabled.yaml"))

    assert "safe_mode::SafeModeComponent" in generated
    assert {"USE_WEBSERVER_OTA", "USE_PREFERENCE_KEY_LOOKUP"} <= _defines()


def test_safe_mode_with_guarded_standby_keeps_native_ota_veto(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("native_ota_standby.yaml"))

    assert "safe_mode::SafeModeComponent" in generated
    assert "XR806StandbyController" in generated
    # Native OTA still has no proven listener quiesce; do not bypass its veto.
    assert "set_http_only(false)" in generated


def test_safe_mode_can_be_explicitly_disabled(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("safe_mode_disabled.yaml"))

    assert "USE_OTA" in _defines()
    assert "safe_mode::SafeModeComponent" not in generated
    assert "should_enter_safe_mode(" not in generated


def test_rtc_storage_is_rejected_on_xr806(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    config = component_config_path("safe_mode_explicit.yaml").read_text()
    CORE.config_path = tmp_path / "xr806_safe_mode_rtc.yaml"
    CORE.config_path.write_text(config.replace("storage: flash", "storage: rtc"))

    assert read_config({}) is None
    assert "'rtc' storage is not supported on this platform" in capsys.readouterr().out
