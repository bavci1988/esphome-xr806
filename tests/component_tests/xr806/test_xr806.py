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


def test_uart_uses_the_configured_frame_format(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("uart.yaml"))

    assert {"USE_UART_DEBUGGER", "USE_XR806"} <= defines()
    assert "uart::XR806UARTComponent" in generated
    assert "set_baud_rate(9600)" in generated
    assert "set_data_bits(7)" in generated
    assert "set_parity(uart::UART_CONFIG_PARITY_EVEN)" in generated
    assert "set_stop_bits(2)" in generated
    assert "set_rx_buffer_size(512)" in generated


@pytest.mark.parametrize(
    ("pin_line", "message"),
    [
        ("  tx_pin: PB0\n", "XR806 UART0 tx_pin must be PB0"),
        ("  rx_pin: PB1\n", "XR806 UART0 rx_pin must be PB1"),
    ],
)
def test_uart_rejects_pins_other_than_uart0(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    pin_line: str,
    message: str,
) -> None:
    config = component_config_path("uart.yaml").read_text()
    CORE.config_path = tmp_path / "uart.yaml"
    CORE.config_path.write_text(config.replace(pin_line, pin_line[:-4] + "PA19\n"))

    assert read_config({}) is None
    assert message in capsys.readouterr().out


@pytest.mark.parametrize(
    ("addition", "message"),
    [
        ("  rx_buffer_size: 0\n", None),
        ("  rx_buffer_size: 4097\n", "XR806 UART RX buffer must be 1..4096 bytes"),
    ],
)
def test_uart_rejects_invalid_rx_buffer(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    addition: str,
    message: str,
) -> None:
    config = component_config_path("uart.yaml").read_text()
    CORE.config_path = tmp_path / "uart.yaml"
    CORE.config_path.write_text(config.replace("  rx_buffer_size: 512\n", addition))

    assert read_config({}) is None
    output = capsys.readouterr().out
    if message is not None:
        assert message in output


def test_uart_rejects_duplicate_uart0(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    config = component_config_path("uart.yaml").read_text()
    CORE.config_path = tmp_path / "uart.yaml"
    start = config.index("uart:\n")
    # Deliberately omit the interval action: this test exercises bus ownership.
    config = (
        config[:start]
        + "uart:\n"
        + "  - id: uart_bus\n    tx_pin: PB0\n    baud_rate: 9600\n"
        + "  - id: duplicate_uart\n    rx_pin: PB1\n    baud_rate: 9600\n"
    )
    CORE.config_path.write_text(config)

    assert read_config({}) is None
    assert "XR806 exposes only one UART0 bus" in capsys.readouterr().out


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


def test_prometheus_with_shared_web_server(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("prometheus.yaml"))

    assert {
        "USE_PROMETHEUS",
        "USE_WEBSERVER",
        "USE_NETWORK",
        "USE_SOCKET_IMPL_LWIP_SOCKETS",
    } <= defines()
    assert "prometheus::PrometheusHandler" in generated
    assert "web_server_xr806::WebServerXR806" in generated
    assert "set_include_internal(true)" in generated
    assert "add_label_id(" in generated
    assert "add_label_name(" in generated


def test_prometheus_without_web_server(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("prometheus_only.yaml"))

    assert "prometheus::PrometheusHandler" in generated
    assert "web_server_xr806::WebServerXR806" in generated


def test_mdns_announces_api_web_server_and_prometheus(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generate_main(component_config_path("prometheus_api.yaml"))

    counts = {
        str(define) for define in CORE.defines if define.name == "MDNS_SERVICE_COUNT"
    }
    assert counts == {"MDNS_SERVICE_COUNT=3"}


def test_mqtt_uses_the_native_backend(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("mqtt.yaml"))

    assert {
        "USE_MQTT",
        "USE_MQTT_SESSION_PERSISTENCE",
        "USE_SOCKET_IPV4_RESOLVE",
        "USE_SOCKET_IMPL_LWIP_SOCKETS",
        "USE_JSON",
    } <= defines()
    assert "MQTT_MAX_PAYLOAD_SIZE=8192" in {str(define) for define in CORE.defines}
    assert "mqtt::MQTTClientComponent" in generated
    assert "mqtt_subscribe::MQTTSubscribeSensor" in generated


def test_mqtt_with_standby_registers_its_participant(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("mqtt_standby.yaml"))

    assert {"USE_MQTT", "USE_XR806_STANDBY_CONTROLLER"} <= defines()
    assert "xr806::XR806MQTTStandby" in generated
    assert "set_no_mqtt(false)" in generated


def test_standby_without_mqtt_needs_no_mqtt_owner(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("http_request_standby.yaml"))

    assert "set_no_mqtt(true)" in generated
    assert "XR806MQTTStandby" not in generated


def test_gpio_pins(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("gpio.yaml"))

    assert "xr806::XR806GPIOPin" in generated
    assert "set_pin(20)" in generated
    assert "set_pin(19)" in generated
    assert "set_inverted(true)" in generated
    assert "(gpio::Flags::FLAG_INPUT | gpio::Flags::FLAG_PULLUP)" in generated
    assert "set_interrupt_type(gpio::INTERRUPT_FALLING_EDGE)" in generated


@pytest.mark.parametrize(
    ("pin_block", "message"),
    [
        ("      number: PA21\n", "GPIO PA21 is reserved: UART2 RX"),
        ("      number: PB5\n", "GPIO PB5 is reserved: Flash/XIP"),
        ("      number: PA0\n", "GPIO PA0 is not exposed on the WXU module"),
        ("      number: 20\n", "XR806 pins must use PAx or PBx notation"),
        (
            (
                "      number: PA20\n      mode:\n        output: true\n"
                "        pullup: true\n        pulldown: true\n"
            ),
            "Pull-up and pull-down cannot both be enabled",
        ),
    ],
)
def test_gpio_rejects_invalid_pins(
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    pin_block: str,
    message: str,
) -> None:
    config = component_config_path("gpio.yaml").read_text()
    CORE.config_path = tmp_path / "gpio.yaml"
    CORE.config_path.write_text(
        config.replace("      number: PA20\n      inverted: true\n", pin_block)
    )

    assert read_config({}) is None
    assert message in capsys.readouterr().out


def test_gpio_accepts_input_and_output() -> None:
    from esphome.components.xr806.gpio import validate_gpio_mode

    mode = {"input": True, "output": True, "pullup": False, "pulldown": False}
    assert validate_gpio_mode(mode) == mode


@pytest.mark.parametrize("gpio_pin", ["PA19", "PA20"])
def test_standby_wake_pin_is_dedicated(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    gpio_pin: str,
) -> None:
    # The standby HAL configures PA20 directly, so the normal GPIO pin
    # duplicate-use validation does not detect a second consumer.
    config = (
        "esphome:\n  name: xr806-wake-pin-test\n"
        "xr806:\n  board: wxu\n  lowpower: true\n"
        "  standby:\n    enabled: true\n    wake_pin: PA20\n    wake_edge: falling\n"
        "switch:\n  - platform: gpio\n    id: test_switch\n"
        f"    pin: {gpio_pin}\n"
    )
    CORE.config_path = tmp_path / "wake_gpio.yaml"
    CORE.config_path.write_text(config)

    result = read_config({})
    if gpio_pin == "PA20":
        assert result is None
        assert (
            "xr806.standby.wake_pin PA20 must be dedicated" in capsys.readouterr().out
        )
    else:
        assert result is not None


def test_standby_wake_pin_rejects_allow_other_uses(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    config = (
        "esphome:\n  name: xr806-wake-pin-sharing\n"
        "xr806:\n  board: wxu\n  lowpower: true\n"
        "  standby:\n    enabled: true\n    wake_pin: PA20\n    wake_edge: falling\n"
        "binary_sensor:\n  - platform: gpio\n    id: wake_sensor\n"
        "    pin:\n      number: PA20\n      allow_other_uses: true\n"
    )
    CORE.config_path = tmp_path / "wake_shared.yaml"
    CORE.config_path.write_text(config)

    assert read_config({}) is None
    assert "wake_pin PA20 must be dedicated" in capsys.readouterr().out


def test_mqtt_tls_with_ca_and_client_certificate(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("mqtt_tls.yaml"))

    assert "USE_MQTT_TLS" in defines()
    assert {"USE_MQTT_TLS_INSECURE", "USE_MQTT_CERT_BUNDLE"}.isdisjoint(defines())
    assert "set_ca_certificate(" in generated
    assert "set_cl_certificate(" in generated
    assert "set_cl_key(" in generated
    assert "set_skip_cert_cn_check(true)" in generated
    assert "set_broker_port(8883)" in generated


def test_mqtt_tls_without_verification(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    generated = generate_main(component_config_path("mqtt_tls_insecure.yaml"))

    assert {"USE_MQTT_TLS", "USE_MQTT_TLS_INSECURE"} <= defines()
    assert "set_ca_certificate(" not in generated


def test_mqtt_tls_requires_a_certificate_authority(
    component_config_path: Callable[[str], Path],
    capsys: pytest.CaptureFixture[str],
) -> None:
    CORE.config_path = component_config_path("mqtt_tls_no_ca.yaml")

    assert read_config({}) is None
    assert "XR806 has no certificate bundle" in capsys.readouterr().out


def test_standby_times_accept_time_periods(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
) -> None:
    config = component_config_path("native_ota_standby.yaml").read_text()
    config = config.replace(
        "    enabled: true\n",
        "    enabled: true\n    wake_after: 2s\n    quiet_time: 300ms\n    min_awake_time: 1min\n",
    )
    path = tmp_path / "standby_times.yaml"
    path.write_text(config)
    generated = generate_main(path)

    assert "set_wake_after_ms(2000)" in generated
    assert "set_quiet_ms(300)" in generated
    assert "set_min_awake_ms(60000)" in generated


def test_standby_old_millisecond_keys_still_work(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    config = component_config_path("native_ota_standby.yaml").read_text()
    config = config.replace(
        "    enabled: true\n",
        "    enabled: true\n    wake_after_ms: 2500\n    quiet_ms: 400\n",
    )
    path = tmp_path / "standby_old_keys.yaml"
    path.write_text(config)
    generated = generate_main(path)

    assert "set_wake_after_ms(2500)" in generated
    assert "set_quiet_ms(400)" in generated
    assert "'wake_after_ms' is deprecated, use 'wake_after'" in caplog.text


def test_standby_wake_pin_must_be_a_wake_input(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    config = (
        "esphome:\n  name: xr806-wake-pin-invalid\n"
        "xr806:\n  board: wxu\n  lowpower: true\n"
        "  standby:\n    enabled: true\n    wake_pin: PB0\n    wake_edge: falling\n"
    )
    CORE.config_path = tmp_path / "wake_invalid.yaml"
    CORE.config_path.write_text(config)

    assert read_config({}) is None
    assert (
        "wake_pin must be one of PA11, PA12, PA13, PA19, PA20"
        in capsys.readouterr().out
    )


@pytest.mark.parametrize(("pin", "valid"), [("PB4", True), ("PB5", False)])
def test_flash_pins_stay_reserved_except_pb4(pin: str, valid: bool) -> None:
    from esphome.components.xr806.gpio import validate_gpio_pin
    import esphome.config_validation as cv

    if valid:
        assert validate_gpio_pin(pin) == 36
    else:
        with pytest.raises(cv.Invalid, match="Flash/XIP"):
            validate_gpio_pin(pin)
