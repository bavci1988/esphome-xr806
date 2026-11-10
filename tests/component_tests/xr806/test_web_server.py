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


def test_webserver_standby_requires_quiesce_owner(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    with pytest.raises(Exception, match="HTTP listener"):
        generate_main(component_config_path("web_server_standby_rejected.yaml"))


def test_webserver_ota_fails_closed(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    with pytest.raises(Exception, match="safe flash writer"):
        generate_main(component_config_path("web_server_ota_rejected.yaml"))
