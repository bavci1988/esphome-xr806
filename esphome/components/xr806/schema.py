"""XR806 power management options: standby and the WLAN power settings."""

from esphome import final_validate as fv, pins
import esphome.codegen as cg
from esphome.components.const import CONF_ENABLED
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_KEEPALIVE,
    CONF_PLATFORM,
    CONF_PROTOCOL,
    CONF_REBOOT_TIMEOUT,
    CONF_UART_ID,
)
from esphome.core import CORE, ID
from esphome.types import ConfigType

CONF_WLAN = "wlan"
CONF_NETWORK_SUSPEND = "network_suspend"
CONF_STAY_CONNECTED = "stay_connected"
CONF_DTIM = "dtim"
CONF_LISTEN_INTERVAL = "listen_interval"
CONF_STANDBY = "standby"
CONF_OTA_GUARD_ID = "ota_guard_id"
CONF_API_STANDBY_ID = "api_standby_id"
CONF_HTTP_STANDBY_ID = "http_standby_id"
CONF_MQTT_STANDBY_ID = "mqtt_standby_id"
CONF_MDNS_STANDBY_ID = "mdns_standby_id"
CONF_UART_STANDBY_ID = "uart_standby_id"
CONF_TUYA_HOST_ID = "tuya_host_id"
CONF_QUIET_TIME = "quiet_time"
CONF_MIN_AWAKE_TIME = "min_awake_time"
CONF_REQUIRE_EXTERNAL_SYNC = "require_external_sync"
CONF_WAKE_AFTER = "wake_after"
CONF_WAKE_PIN = "wake_pin"
CONF_WAKE_EDGE = "wake_edge"
# safe_mode counts from its own setup(), a little after boot.
BOOT_HOLD_MARGIN_MS = 2000

xr806_ns = cg.esphome_ns.namespace("xr806")
XR806WiFiStandby = xr806_ns.class_("XR806WiFiStandby", cg.Component)
XR806APIStandby = xr806_ns.class_("XR806APIStandby", cg.Component)
XR806StandbyController = xr806_ns.class_("XR806StandbyController", cg.Component)
XR806OTAStandbyGuard = xr806_ns.class_("XR806OTAStandbyGuard", cg.Component)
XR806HTTPStandby = xr806_ns.class_("XR806HTTPStandby", cg.Component)
XR806MQTTStandby = xr806_ns.class_("XR806MQTTStandby", cg.Component)
XR806MDNSStandby = xr806_ns.class_("XR806MDNSStandby", cg.Component)
XR806UARTStandby = xr806_ns.class_("XR806UARTStandby", cg.Component)
XR806TuyaLowPowerHost = xr806_ns.class_("XR806TuyaLowPowerHost", cg.Component)

# PA pins that can wake the chip from standby; the other wake inputs are not on the WXU pads
# or are used by UART2.
_WAKE_GPIOS = (11, 12, 13, 19, 20)


def validate_wake_pin(value: object) -> int:
    """A standby wake input, written like any XR806 pin (PA20); the GPIO number."""
    number = value
    if isinstance(value, str):
        name = value.upper()
        if name.startswith("GPIO"):
            name = name[4:]
        elif name.startswith("PA"):
            name = name[2:]
        try:
            number = int(name)
        except ValueError:
            number = None
    if not isinstance(number, int) or isinstance(number, bool):
        raise cv.Invalid(f"Invalid wake pin: {value}")
    if number not in _WAKE_GPIOS:
        raise cv.Invalid(
            "wake_pin must be one of " + ", ".join(f"PA{gpio}" for gpio in _WAKE_GPIOS)
        )
    return number


# Radio settings for the standard wifi: component. DTIM and the listen interval
# apply when wifi: power_save_mode is LIGHT or HIGH.
XR806_WLAN_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806WiFiStandby),
        cv.Optional(CONF_DTIM, default=1): cv.int_range(min=1, max=30),
        cv.Optional(CONF_LISTEN_INTERVAL, default=0): cv.int_range(min=0, max=30),
        cv.Optional(CONF_NETWORK_SUSPEND, default=False): cv.boolean,
        # Standby keeps the station connected; received data and the wake timer wake the chip.
        cv.Optional(CONF_STAY_CONNECTED, default=False): cv.boolean,
    }
)

# The standby coordinator does not implement any Tuya/MCU commands or UART autobaud.
# Every configured transport has an owner that must clear its fail-closed blocker.
STANDBY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806StandbyController),
        cv.GenerateID(CONF_OTA_GUARD_ID): cv.declare_id(XR806OTAStandbyGuard),
        cv.GenerateID(CONF_API_STANDBY_ID): cv.declare_id(XR806APIStandby),
        cv.GenerateID(CONF_HTTP_STANDBY_ID): cv.declare_id(XR806HTTPStandby),
        cv.GenerateID(CONF_MQTT_STANDBY_ID): cv.declare_id(XR806MQTTStandby),
        cv.GenerateID(CONF_MDNS_STANDBY_ID): cv.declare_id(XR806MDNSStandby),
        cv.GenerateID(CONF_UART_STANDBY_ID): cv.declare_id(XR806UARTStandby),
        cv.Required(CONF_ENABLED): cv.boolean,
        # The hardware timer reaches about 18.6 hours.
        cv.Optional(CONF_WAKE_AFTER, default="3s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(milliseconds=500),
                max=cv.TimePeriod(hours=12),
            ),
        ),
        cv.Optional(CONF_QUIET_TIME, default="250ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(milliseconds=200),
                max=cv.TimePeriod(milliseconds=30000),
            ),
        ),
        cv.Optional(CONF_MIN_AWAKE_TIME, default="5s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(milliseconds=1000),
                max=cv.TimePeriod(milliseconds=60000),
            ),
        ),
        cv.Optional(CONF_WAKE_PIN): validate_wake_pin,
        cv.Optional(CONF_WAKE_EDGE): cv.one_of("falling", "rising", lower=True),
        cv.Optional(CONF_REQUIRE_EXTERNAL_SYNC, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)

COMPONENT_SCHEMA = {
    cv.GenerateID(CONF_TUYA_HOST_ID): cv.declare_id(XR806TuyaLowPowerHost),
    cv.Optional(CONF_WLAN, default={}): XR806_WLAN_SCHEMA,
    cv.Optional(CONF_STANDBY): STANDBY_SCHEMA,
}


def _final_validate_options(config: ConfigType) -> ConfigType:
    wlan = config[CONF_WLAN]
    standby = config.get(CONF_STANDBY)
    if wlan[CONF_NETWORK_SUSPEND] and standby is None:
        raise cv.Invalid(
            "xr806.wlan.network_suspend requires xr806.standby",
            path=[CONF_WLAN, CONF_NETWORK_SUSPEND],
        )
    if wlan[CONF_STAY_CONNECTED]:
        if standby is None:
            raise cv.Invalid(
                "xr806.wlan.stay_connected requires xr806.standby",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
        if wlan[CONF_NETWORK_SUSPEND]:
            raise cv.Invalid(
                "xr806.wlan.stay_connected and network_suspend exclude each other",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
        # Only a radio in power save mode stays associated while the CPU sleeps.
        wifi_config = fv.full_config.get().get("wifi")
        if wifi_config is None:
            raise cv.Invalid(
                "xr806.wlan.stay_connected requires wifi",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
        if str(wifi_config.get("power_save_mode", "NONE")).upper() == "NONE":
            raise cv.Invalid(
                "xr806.wlan.stay_connected requires wifi power_save_mode light or high",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
    if standby is None:
        return config
    if not standby[CONF_ENABLED]:
        raise cv.Invalid("Remove xr806.standby when disabled", path=[CONF_STANDBY])
    if (CONF_WAKE_PIN in standby) != (CONF_WAKE_EDGE in standby):
        raise cv.Invalid(
            "xr806.standby requires explicit wake_pin and wake_edge together",
            path=[CONF_STANDBY],
        )
    if (gpio := standby.get(CONF_WAKE_PIN)) is not None and (
        pins.PIN_SCHEMA_REGISTRY.get_count("xr806", "xr806", gpio)
    ):
        # The standby call configures wake_pin directly, so ESPHome's duplicate-pin check cannot see it.
        raise cv.Invalid(
            f"xr806.standby.wake_pin PA{gpio} must be dedicated: "
            "another component already uses this GPIO",
            path=[CONF_STANDBY, CONF_WAKE_PIN],
        )
    _final_validate_standby_reachable(config)
    return config


def _final_validate_standby_reachable(config: ConfigType) -> None:
    """Refuse configurations in which a service owner never lets the chip sleep."""
    full_config = fv.full_config.get()
    wlan = config[CONF_WLAN]
    uart_configs = full_config.get("uart", [])
    # UART_BUSY has a single owner, and only the Tuya low_power host owns the UART.
    if len(uart_configs) > 1:
        raise cv.Invalid("xr806.standby supports one uart bus", path=[CONF_STANDBY])
    tuya_config = full_config.get("tuya")
    if uart_configs and not (
        isinstance(tuya_config, dict) and tuya_config.get(CONF_PROTOCOL) == "low_power"
    ):
        raise cv.Invalid(
            "xr806.standby with a uart bus needs tuya with protocol: low_power, "
            "which decides with the MCU when the UART is idle",
            path=[CONF_STANDBY],
        )
    if "wifi" in full_config and not (
        wlan[CONF_NETWORK_SUSPEND] or wlan[CONF_STAY_CONNECTED]
    ):
        raise cv.Invalid(
            "xr806.standby with wifi needs xr806.wlan network_suspend or stay_connected; "
            "otherwise the WLAN link never lets the chip sleep",
            path=[CONF_WLAN],
        )
    if (api_config := full_config.get("api")) is not None and (
        "outgoing_connection" in api_config
    ):
        raise cv.Invalid(
            "xr806.standby does not support api outgoing_connection, "
            "which can start network work at any time",
            path=[CONF_STANDBY],
        )
    mqtt_config = full_config.get("mqtt")
    if wlan[CONF_STAY_CONNECTED] and mqtt_config is not None:
        keepalive = mqtt_config[CONF_KEEPALIVE]
        if (
            config[CONF_STANDBY][CONF_WAKE_AFTER].total_milliseconds
            >= keepalive.total_milliseconds
        ):
            raise cv.Invalid(
                f"With mqtt and xr806.wlan.stay_connected, xr806.standby.wake_after must be "
                f"shorter than the mqtt keepalive ({keepalive}), or the broker drops the connection",
                path=[CONF_STANDBY, CONF_WAKE_AFTER],
            )
    if not wlan[CONF_NETWORK_SUSPEND]:
        return
    if any(
        item.get(CONF_PLATFORM) != "web_server" for item in full_config.get("ota", [])
    ):
        raise cv.Invalid(
            "xr806.wlan.network_suspend works only with ota platform web_server: the "
            "native OTA listener cannot be parked; use stay_connected instead",
            path=[CONF_WLAN, CONF_NETWORK_SUSPEND],
        )
    for domain, domain_config in (("api", api_config), ("mqtt", mqtt_config)):
        if (
            domain_config is not None
            and domain_config[CONF_REBOOT_TIMEOUT].total_milliseconds
        ):
            raise cv.Invalid(
                f"With xr806.wlan.network_suspend, no {domain} connection lasts through a "
                f"standby: set {domain} reboot_timeout: 0s, or the device keeps rebooting",
                path=[CONF_WLAN, CONF_NETWORK_SUSPEND],
            )


FINAL_VALIDATE_SCHEMA = _final_validate_options


async def _tuya_low_power_host(host_id: ID, tuya_config: ConfigType) -> None:
    """Connect a Tuya hub running protocol low_power to XR806 power management."""
    tuya = await cg.get_variable(tuya_config[CONF_ID])
    uart = await cg.get_variable(tuya_config[CONF_UART_ID])
    cg.add_global(
        cg.RawStatement('#include "esphome/components/xr806/tuya_low_power_host.h"')
    )
    host = cg.new_Pvariable(host_id, tuya, uart)
    await cg.register_component(host, {})


async def _participant(header: str, var_id: ID, *args: object) -> cg.MockObj:
    cg.add_global(cg.RawStatement(f'#include "esphome/components/xr806/{header}"'))
    var = cg.new_Pvariable(var_id, *args)
    await cg.register_component(var, {})
    return var


async def _standby_to_code(config: ConfigType) -> None:
    cfg = config[CONF_STANDBY]
    cg.add_define("USE_XR806_STANDBY_CONTROLLER")
    cg.add_global(
        cg.RawStatement('#include "esphome/components/xr806/standby_controller.h"')
    )
    controller = cg.new_Pvariable(cfg[CONF_ID])
    cg.add(controller.set_wake_after_ms(cfg[CONF_WAKE_AFTER].total_milliseconds))
    cg.add(controller.set_quiet_ms(cfg[CONF_QUIET_TIME].total_milliseconds))
    cg.add(controller.set_min_awake_ms(cfg[CONF_MIN_AWAKE_TIME].total_milliseconds))
    if (safe_mode := CORE.config.get("safe_mode")) is not None:
        # Stay awake until safe_mode has seen the boot through, so a crash in the first standby counts
        # as a failed boot only if it really is one, and OTA stays reachable after every restart.
        cg.add(
            controller.set_boot_hold_ms(
                safe_mode["boot_is_good_after"].total_milliseconds + BOOT_HOLD_MARGIN_MS
            )
        )
    if (wake_pin := cfg.get(CONF_WAKE_PIN)) is not None:
        cg.add(controller.set_wake_pin(wake_pin))
    cg.add(controller.set_wake_falling(cfg.get(CONF_WAKE_EDGE, "falling") == "falling"))
    # Transports which are absent can never have work in flight. Present transports keep
    # their initial busy vote until their owner votes idle.
    has_http = any(domain in CORE.config for domain in ("web_server", "captive_portal"))
    cg.add(controller.set_no_uart("uart" not in CORE.config))
    cg.add(controller.set_no_network("wifi" not in CORE.config))
    cg.add(controller.set_no_api("api" not in CORE.config))
    cg.add(controller.set_no_ota("ota" not in CORE.config))
    cg.add(controller.set_no_mdns("mdns" not in CORE.config))
    cg.add(controller.set_no_http(not has_http))
    cg.add(controller.set_no_mqtt("mqtt" not in CORE.config))
    cg.add(controller.set_external_sync_required(cfg[CONF_REQUIRE_EXTERNAL_SYNC]))
    cg.add(controller.set_require_uart_participant("uart" in CORE.config))
    cg.add(controller.set_stay_connected(config[CONF_WLAN][CONF_STAY_CONNECTED]))
    await cg.register_component(controller, cfg)

    if uart_configs := CORE.config.get("uart"):
        cg.add_define("USE_XR806_UART_STANDBY")
        uart = await cg.get_variable(uart_configs[0][CONF_ID])
        await _participant("uart_standby.h", cfg[CONF_UART_STANDBY_ID], uart)
    if has_http:
        cg.add_define("USE_XR806_HTTP_STANDBY")
        await _participant("http_standby.h", cfg[CONF_HTTP_STANDBY_ID])
    if "api" in CORE.config:
        await _participant("api_standby.h", cfg[CONF_API_STANDBY_ID])
    if "mqtt" in CORE.config:
        await _participant("mqtt_standby.h", cfg[CONF_MQTT_STANDBY_ID])
    if "mdns" in CORE.config:
        await _participant("mdns_standby.h", cfg[CONF_MDNS_STANDBY_ID])
    if ota_configs := CORE.config.get("ota"):
        cg.add_define("USE_OTA_STATE_LISTENER")
        ota_guard = await _participant("standby_ota_guard.h", cfg[CONF_OTA_GUARD_ID])
        # Web server uploads share the HTTP listener and its vote; native OTA has a listener of its own.
        cg.add(
            ota_guard.set_http_only(
                all(item.get("platform") == "web_server" for item in ota_configs)
            )
        )


async def component_extra_to_code(config: ConfigType) -> None:
    if CONF_STANDBY in config:
        await _standby_to_code(config)

    if "wifi" in CORE.config:
        wlan = config[CONF_WLAN]
        cg.add_global(cg.RawStatement("#include <xr806_pm.h>"))
        cg.add(
            cg.RawExpression(
                f"xr806_wlan_set_power_save({wlan[CONF_DTIM]}, {wlan[CONF_LISTEN_INTERVAL]})"
            )
        )
        if CONF_STANDBY in config:
            # Owns NETWORK_BUSY and the WLAN wakelock; parks the link only with network_suspend.
            await _participant(
                "wifi_standby.h",
                wlan[CONF_ID],
                wlan[CONF_NETWORK_SUSPEND],
                wlan[CONF_STAY_CONNECTED],
            )

    tuya_config = CORE.config.get("tuya")
    # An AutoLoad marker (tuya loaded only as a base class) is not a configuration.
    if isinstance(tuya_config, dict) and tuya_config.get(CONF_PROTOCOL) == "low_power":
        await _tuya_low_power_host(config[CONF_TUYA_HOST_ID], tuya_config)
