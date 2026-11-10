"""Tests for the XR806 platform configuration."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.components.xr806 import compiler_build_flags
from esphome.config import read_config
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


def test_logger_uses_the_console_uart(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("logger.yaml"))

    assert "USE_LOGGER" in defines()
    # UART2 is the C++ default on XR806, so no setter is generated.
    assert "set_uart_selection" not in generated
    assert "-DESPHOME_LOG_LEVEL=ESPHOME_LOG_LEVEL_DEBUG" in CORE.build_flags


def test_logger_rejects_the_mcu_uart(
    component_config_path: Callable[[str], Path],
    capsys: pytest.CaptureFixture[str],
) -> None:
    CORE.config_path = component_config_path("logger_uart0.yaml")

    assert read_config({}) is None
    assert "UART2" in capsys.readouterr().out


def test_debug_uses_standard_entities(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("debug.yaml"))

    assert {"USE_DEBUG", "USE_LOGGER", "USE_SENSOR", "USE_TEXT_SENSOR"} <= defines()
    assert "debug::DebugComponent" in generated
    assert "set_update_interval(5000)" in generated
    assert "set_free_sensor(" in generated
    assert "set_loop_time_sensor(" in generated
    assert "set_cpu_frequency_sensor(" in generated
    assert "set_device_info_text_sensor(" in generated
    assert "set_reset_reason_text_sensor(" in generated


def test_debug_rejects_unsupported_heap_block(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    config = component_config_path("debug.yaml").read_text()
    CORE.config_path = tmp_path / "debug.yaml"
    CORE.config_path.write_text(config.replace("    free:", "    block:"))

    assert read_config({}) is None
    assert (
        "XR806 does not report the largest free heap block" in capsys.readouterr().out
    )


def test_compiler_build_flags_reach_the_make_adapter() -> None:
    flags = {
        "-DESPHOME_LOG_LEVEL=ESPHOME_LOG_LEVEL_DEBUG",
        "-Wno-sign-compare",
        "-fno-exceptions",
        "-Wl,--wrap=x",
    }

    assert compiler_build_flags(flags) == [
        "-DESPHOME_LOG_LEVEL=ESPHOME_LOG_LEVEL_DEBUG",
        "-Wno-sign-compare",
        "-fno-exceptions",
    ]


def test_udp_packet_transport(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("udp.yaml"))

    assert {"USE_UDP", "USE_SOCKET_IMPL_LWIP_SOCKETS"} <= defines()
    assert "udp::UDPComponent" in generated
    assert "packet_transport" in generated


def test_sntp_with_timezone(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("sntp.yaml"))

    assert {"USE_TIME", "USE_TIME_TIMEZONE"} <= defines()
    assert "sntp::SNTPComponent" in generated
    assert '"0.pool.ntp.org"' in generated


def test_syslog_over_udp_with_sntp_time(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("syslog.yaml"))

    assert {"USE_UDP", "USE_TIME"} <= defines()
    assert "syslog::Syslog" in generated


def test_http_request_with_ota_and_update(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("http_request.yaml"))

    assert {"USE_UPDATE", "USE_OTA"} <= defines()
    assert "http_request::HttpRequestXR806" in generated
    assert "http_request::OtaHttpRequestComponent" in generated
    assert "http_request::HttpRequestUpdate" in generated
    assert 'ESPHOME_VARIANT="XR806"' in {str(define) for define in CORE.defines}


def test_http_request_blocks_standby_and_sends_put(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("http_request_standby.yaml"))

    assert {
        "USE_XR806_STANDBY_CONTROLLER",
        "USE_XR806_STANDBY_HTTP_CLIENT",
    } <= defines()
    assert '"PUT"' in generated
    assert '"DELETE"' in generated


def test_http_request_rejects_certificate_verification(
    component_config_path: Callable[[str], Path],
    capsys: pytest.CaptureFixture[str],
) -> None:
    CORE.config_path = component_config_path("http_request_verify_ssl.yaml")

    assert read_config({}) is None
    assert "certificate verification on XR806" in capsys.readouterr().out


def test_homeassistant_time_with_timezone(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("homeassistant_time.yaml"))

    assert {"USE_API", "USE_TIME", "USE_TIME_TIMEZONE"} <= defines()
    assert "homeassistant::HomeassistantTime" in generated
