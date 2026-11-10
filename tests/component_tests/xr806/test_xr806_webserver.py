"""Code-generation checks for normal ESPHome web_server on native XR806.

These tests validate configuration and generated defines, not ARM linkage,
Wi-Fi association, browser events, persisted credentials or safe sleep.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.core import CORE


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_web_server_uses_standard_yaml(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("web_server.yaml"))
    assert {"USE_XR806", "USE_WEBSERVER", "USE_WEBSERVER_AUTH"} <= _defines()
    assert "USE_WEBSERVER_OTA" not in _defines()


def test_web_server_with_standard_captive_portal(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("web_server_captive.yaml"))
    assert {"USE_XR806", "USE_WEBSERVER", "USE_CAPTIVE_PORTAL", "USE_WEBSERVER_LOCAL"} <= _defines()
    assert "USE_WEBSERVER_OTA" not in _defines()


def test_webserver_standby_requires_quiesce_owner() -> None:
    from esphome.components.xr806 import _final_validate_uart
    from esphome import final_validate as fv

    token = fv.full_config.set({"web_server": {}, "wifi": {}})
    try:
        with pytest.raises(Exception, match="HTTP listener"):
            _final_validate_uart({"standby": {}})
    finally:
        fv.full_config.reset(token)


def test_webserver_ota_fails_closed() -> None:
    from esphome.components.web_server.ota import _web_server_ota_final_validate
    from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_XR806

    previous = CORE.data.get(KEY_CORE)
    CORE.data[KEY_CORE] = {KEY_TARGET_PLATFORM: PLATFORM_XR806}
    try:
        with pytest.raises(Exception, match="safe flash writer"):
            _web_server_ota_final_validate({})
    finally:
        if previous is None:
            CORE.data.pop(KEY_CORE, None)
        else:
            CORE.data[KEY_CORE] = previous
