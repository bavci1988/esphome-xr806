"""Code-generation checks for normal ESPHome web_server on native XR806.

These tests validate configuration and generated defines, not ARM linkage,
Wi-Fi association, browser events, persisted credentials or safe sleep.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

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
    assert {
        "USE_XR806",
        "USE_WEBSERVER",
        "USE_CAPTIVE_PORTAL",
        "USE_WEBSERVER_LOCAL",
    } <= _defines()
    assert "USE_WEBSERVER_OTA" not in _defines()


def test_http_standby_registers_network_owner(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("web_server_standby.yaml"))
    assert {"USE_XR806_HTTP_STANDBY", "USE_XR806_STANDBY_CONTROLLER"} <= _defines()
    assert "set_no_http(false)" in generated


def test_web_server_ota_standard_platform(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("web_server_ota_enabled.yaml"))
    assert "USE_WEBSERVER_OTA" in _defines()
    assert "WebServerOTAComponent" in generated


def test_http_ota_and_standby_share_the_same_listener(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("web_server_ota_standby.yaml"))
    assert {"USE_WEBSERVER_OTA", "USE_XR806_HTTP_STANDBY"} <= _defines()
    assert "set_http_only(true)" in generated


def test_native_ota_fails_closed_without_transport_quiesce(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    # Existing Tuya + native OTA configs must keep compiling; native OTA's
    # standby participant refuses PM when it cannot prove socket shutdown.
    generated = generate_main(component_config_path("native_ota_standby.yaml"))
    assert "set_http_only(false)" in generated
