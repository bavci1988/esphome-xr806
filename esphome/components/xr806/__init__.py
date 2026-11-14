"""Native ESPHome platform for the Allwinner/XRadio XR806 SoC."""

import logging
from pathlib import Path

from esphome import final_validate as fv, pins
import esphome.codegen as cg
from esphome.components.const import CONF_ENABLED
import esphome.config_validation as cv
from esphome.const import (
    CONF_BOARD,
    CONF_ENCRYPTION,
    CONF_ID,
    CONF_KEY,
    CONF_PERIOD,
    CONF_PROTOCOL,
    CONF_UART_ID,
    KEY_CORE,
    KEY_FRAMEWORK_VERSION,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    Framework,
    ThreadModel,
)
from esphome.core import CORE, ID, EsphomeError
from esphome.types import ConfigType

CODEOWNERS = ["@bavci1988"]
# The preferences syncer writes pending preferences every 60 s and at shutdown.
AUTO_LOAD = ["preferences"]
DOMAIN = "xr806"
IS_TARGET_PLATFORM = True

OTA_IMAGE_NAME = "firmware.ota.bin"

_LOGGER = logging.getLogger(__name__)

CONF_WLAN = "wlan"
CONF_NETWORK_SUSPEND = "network_suspend"
CONF_STAY_CONNECTED = "stay_connected"
CONF_DTIM = "dtim"
CONF_LISTEN_INTERVAL = "listen_interval"
CONF_STANDBY = "standby"
CONF_LOWPOWER = "lowpower"
CONF_OTA_GUARD_ID = "ota_guard_id"
CONF_OTA_DIAGNOSTIC_READONLY = "ota_diagnostic_readonly"
CONF_API_STANDBY_ID = "api_standby_id"
CONF_HTTP_STANDBY_ID = "http_standby_id"
CONF_MQTT_STANDBY_ID = "mqtt_standby_id"
CONF_TUYA_HOST_ID = "tuya_host_id"
CONF_QUIET_TIME = "quiet_time"
CONF_MIN_AWAKE_TIME = "min_awake_time"
CONF_REQUIRE_EXTERNAL_SYNC = "require_external_sync"
CONF_WAKE_AFTER = "wake_after"
CONF_WAKE_PIN = "wake_pin"
CONF_WAKE_EDGE = "wake_edge"
CONF_API_KEEPALIVE_OFFLOAD = "api_keepalive_offload"
CONF_HOST_SLEEP = "host_sleep"
CONF_WAKE_ON_PEER = "wake_on_peer"

xr806_ns = cg.esphome_ns.namespace("xr806")
XR806WiFiStandby = xr806_ns.class_("XR806WiFiStandby", cg.Component)
XR806APIStandby = xr806_ns.class_("XR806APIStandby", cg.Component)
XR806StandbyController = xr806_ns.class_("XR806StandbyController", cg.Component)
XR806OTAStandbyGuard = xr806_ns.class_("XR806OTAStandbyGuard", cg.Component)
XR806HTTPStandby = xr806_ns.class_("XR806HTTPStandby", cg.Component)
XR806MQTTStandby = xr806_ns.class_("XR806MQTTStandby", cg.Component)
XR806TuyaLowPowerHost = xr806_ns.class_("XR806TuyaLowPowerHost", cg.Component)


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


# PA pins that can wake the chip from standby; PA21..23 are reserved for UART2/boot.
_WAKE_GPIOS = (11, 12, 13, 19, 20)


def _wake_pin_name(number: int) -> str:
    return f"PA{number}"


def validate_wake_pin(value: object) -> int:
    """A standby wake input, written like any XR806 pin (PA20); the GPIO number."""
    # The pin parser lives in gpio.py, which imports this module.
    from .gpio import validate_gpio_pin

    number = validate_gpio_pin(value)
    if number not in _WAKE_GPIOS:
        raise cv.Invalid(
            "wake_pin must be one of "
            + ", ".join(_wake_pin_name(gpio) for gpio in _WAKE_GPIOS)
        )
    return number


# Experiment: the WLAN firmware sends a pre-encrypted PingResponse on the API connection while the CPU sleeps.
# The connection is closed on every wake, because the firmware does not report what it sent.
API_KEEPALIVE_OFFLOAD_SCHEMA = cv.Schema(
    {
        # Home Assistant pings after 20 s without a message from the device.
        cv.Optional(CONF_PERIOD, default="8s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=255)),
        ),
        cv.Optional(CONF_HOST_SLEEP, default=True): cv.boolean,
        cv.Optional(CONF_WAKE_ON_PEER, default=False): cv.boolean,
    }
)


# Production standby coordinator: deliberately does NOT implement any
# Tuya/HC32 commands, UART autobaud, or transport-specific idle voting.
# A firmware with active transports must register lifecycle participants
# and its owners must actually clear their fail-closed PM blockers.
STANDBY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806StandbyController),
        cv.GenerateID(CONF_OTA_GUARD_ID): cv.declare_id(XR806OTAStandbyGuard),
        cv.GenerateID(CONF_API_STANDBY_ID): cv.declare_id(XR806APIStandby),
        cv.GenerateID(CONF_HTTP_STANDBY_ID): cv.declare_id(XR806HTTPStandby),
        cv.GenerateID(CONF_MQTT_STANDBY_ID): cv.declare_id(XR806MQTTStandby),
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
        cv.Optional(CONF_API_KEEPALIVE_OFFLOAD): API_KEEPALIVE_OFFLOAD_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


def _update_core_data(config: ConfigType) -> ConfigType:
    if CONF_LOWPOWER in config:
        # Power management is prepared whenever standby is configured; on its own it did nothing.
        _LOGGER.warning(
            "xr806: 'lowpower' is no longer needed and is ignored; "
            "'standby:' enables power management"
        )
        config.pop(CONF_LOWPOWER)
    if config[CONF_WLAN][CONF_NETWORK_SUSPEND] and CONF_STANDBY not in config:
        raise cv.Invalid(
            "xr806.wlan.network_suspend requires xr806.standby and full PM gating"
        )
    if config[CONF_WLAN][CONF_STAY_CONNECTED]:
        if CONF_STANDBY not in config:
            raise cv.Invalid(
                "xr806.wlan.stay_connected requires xr806.standby",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
        if config[CONF_WLAN][CONF_NETWORK_SUSPEND]:
            raise cv.Invalid(
                "xr806.wlan.stay_connected and network_suspend exclude each other",
                path=[CONF_WLAN, CONF_STAY_CONNECTED],
            )
    if CONF_STANDBY in config:
        standby_cfg = config[CONF_STANDBY]
        if (CONF_WAKE_PIN in standby_cfg) != (CONF_WAKE_EDGE in standby_cfg):
            raise cv.Invalid(
                "xr806.standby requires explicit wake_pin and wake_edge together"
            )
        if (
            CONF_API_KEEPALIVE_OFFLOAD in standby_cfg
            and not config[CONF_WLAN][CONF_STAY_CONNECTED]
        ):
            raise cv.Invalid(
                "xr806.standby.api_keepalive_offload requires xr806.wlan.stay_connected",
                path=[CONF_STANDBY, CONF_API_KEEPALIVE_OFFLOAD],
            )
        if not config[CONF_STANDBY][CONF_ENABLED]:
            raise cv.Invalid("Remove xr806.standby when disabled")
    CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = DOMAIN
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = Framework.XRADIO
    # Version of the pinned XRadio SDK (include/version.h), for require_framework_version().
    CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION] = cv.Version(1, 2, 2)
    return config


# The first versions of these options took a whole number of milliseconds.
_OLD_MS_KEYS = {
    CONF_WAKE_AFTER: "wake_after_ms",
    CONF_QUIET_TIME: "quiet_ms",
    CONF_MIN_AWAKE_TIME: "min_awake_ms",
}


def _old_ms_value(old_key: str) -> object:
    """The old keys took a bare number of milliseconds; give it the unit the new keys need."""

    def validator(config: ConfigType) -> ConfigType:
        if isinstance(config, dict) and isinstance(config.get(old_key), int):
            config = {**config, old_key: f"{config[old_key]}ms"}
        return config

    return validator


def _renamed_ms_keys(*keys: str) -> list:
    """Accept the old millisecond keys under their new names, with a deprecation warning."""
    validators = []
    for key in keys:
        old_key = _OLD_MS_KEYS[key]
        validators.append(_old_ms_value(old_key))
        validators.append(
            cv.rename_key(old_key, key, removed_in="2027.6.0", component="xr806")
        )
    return validators


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            # Former switch for power management, now implied by standby
            cv.Optional(CONF_LOWPOWER): cv.boolean,
            cv.GenerateID(CONF_TUYA_HOST_ID): cv.declare_id(XR806TuyaLowPowerHost),
            cv.Required(CONF_BOARD): cv.one_of("wxu", lower=True),
            cv.Optional(CONF_WLAN, default={}): XR806_WLAN_SCHEMA,
            cv.Optional(CONF_STANDBY): cv.All(
                *_renamed_ms_keys(
                    CONF_WAKE_AFTER, CONF_QUIET_TIME, CONF_MIN_AWAKE_TIME
                ),
                STANDBY_SCHEMA,
            ),
            cv.Optional(CONF_OTA_DIAGNOSTIC_READONLY, default=False): cv.boolean,
        }
    ),
    _update_core_data,
)


# The UART backend drives UART0 only.
XR806_UART0_TX = 32  # PB0, WXU pad 15
XR806_UART0_RX = 33  # PB1, WXU pad 16


def _final_validate_uart(config: ConfigType) -> ConfigType:
    full_config = fv.full_config.get()
    buses = full_config.get("uart", [])
    if len(buses) > 1:
        raise cv.Invalid("XR806 exposes only one UART0 bus (PB0/PB1)", path=["uart"])
    for index, uart in enumerate(buses):
        if not 1 <= uart["rx_buffer_size"] <= 4096:
            raise cv.Invalid(
                "XR806 UART RX buffer must be 1..4096 bytes",
                path=["uart", index, "rx_buffer_size"],
            )
        path = ["uart", index]
        for key, expected, name in (
            ("tx_pin", XR806_UART0_TX, "PB0"),
            ("rx_pin", XR806_UART0_RX, "PB1"),
        ):
            pin = uart.get(key)
            if pin is not None and pin["number"] != expected:
                raise cv.Invalid(f"XR806 UART0 {key} must be {name}", path=[*path, key])
    return config


def _final_validate_wake_pin_ownership(config: ConfigType) -> ConfigType:
    # The PM HAL configures wake_pin directly, so ESPHome's duplicate-pin check cannot see it,
    # and allow_other_uses on another user does not make sharing safe.
    standby = config.get(CONF_STANDBY)
    if standby is None or CONF_WAKE_PIN not in standby:
        return config
    gpio_number = standby[CONF_WAKE_PIN]
    if pins.PIN_SCHEMA_REGISTRY.get_count(DOMAIN, DOMAIN, gpio_number):
        raise cv.Invalid(
            f"xr806.standby.wake_pin {_wake_pin_name(gpio_number)} must be dedicated: "
            "another component already uses this GPIO",
            path=[CONF_STANDBY, CONF_WAKE_PIN],
        )
    return config


def _final_validate_stay_connected(config: ConfigType) -> ConfigType:
    # Only a radio in power save mode stays associated while the CPU sleeps.
    if not config[CONF_WLAN][CONF_STAY_CONNECTED]:
        return config
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
    return config


def _final_validate_api_keepalive_offload(config: ConfigType) -> ConfigType:
    standby = config.get(CONF_STANDBY)
    if standby is None or CONF_API_KEEPALIVE_OFFLOAD not in standby:
        return config
    api_config = fv.full_config.get().get("api")
    if api_config is None or CONF_KEY not in api_config.get(CONF_ENCRYPTION, {}):
        raise cv.Invalid(
            "xr806.standby.api_keepalive_offload requires api with an encryption key",
            path=[CONF_STANDBY, CONF_API_KEEPALIVE_OFFLOAD],
        )
    return config


FINAL_VALIDATE_SCHEMA = cv.All(
    _final_validate_uart,
    _final_validate_api_keepalive_offload,
    _final_validate_wake_pin_ownership,
    _final_validate_stay_connected,
)


async def _tuya_low_power_host(host_id: ID, tuya_config: ConfigType) -> None:
    """Connect a Tuya hub running protocol low_power to XR806 power management."""
    tuya = await cg.get_variable(tuya_config[CONF_ID])
    uart = await cg.get_variable(tuya_config[CONF_UART_ID])
    cg.add_global(
        cg.RawStatement('#include "esphome/components/xr806/tuya_low_power_host.h"')
    )
    host = cg.new_Pvariable(host_id, tuya, uart)
    await cg.register_component(host, {})


async def to_code(config: ConfigType) -> None:
    # The FDCM journal finds stored preferences by key.
    cg.add_define("USE_PREFERENCE_KEY_LOOKUP")
    if config[CONF_OTA_DIAGNOSTIC_READONLY]:
        # ROM/FDCM read-only snapshot: does not enable ESPHome OTA.
        cg.add_define("USE_XR806_OTA_RUNTIME_PROBE")
    if CONF_STANDBY in config:
        # The power management policy and the UART and service wakelocks that standby relies on.
        cg.add_define("USE_XR806_LOWPOWER")
        cfg = config[CONF_STANDBY]
        cg.add_define("USE_XR806_STANDBY_CONTROLLER")
        cg.add_global(
            cg.RawStatement('#include "esphome/components/xr806/standby_controller.h"')
        )
        controller = cg.new_Pvariable(cfg["id"])
        cg.add(controller.set_wake_after_ms(cfg[CONF_WAKE_AFTER].total_milliseconds))
        cg.add(controller.set_quiet_ms(cfg[CONF_QUIET_TIME].total_milliseconds))
        cg.add(controller.set_min_awake_ms(cfg[CONF_MIN_AWAKE_TIME].total_milliseconds))
        if CONF_WAKE_PIN in cfg:
            cg.add(controller.set_wake_pin(cfg[CONF_WAKE_PIN]))
        cg.add(
            controller.set_wake_falling(cfg.get(CONF_WAKE_EDGE, "falling") == "falling")
        )
        # Transports which are absent can never have work in flight. Present
        # transports retain their initial busy vote until their own driver
        # explicitly votes idle; no hardcoded MCU or Tuya protocol assumption.
        cg.add(controller.set_no_uart("uart" not in CORE.config))
        cg.add(controller.set_no_network("wifi" not in CORE.config))
        cg.add(controller.set_no_api("api" not in CORE.config))
        cg.add(controller.set_no_ota("ota" not in CORE.config))
        cg.add(controller.set_no_mdns("mdns" not in CORE.config))
        has_http = any(
            domain in CORE.config for domain in ("web_server", "captive_portal")
        )
        cg.add(controller.set_no_http(not has_http))
        cg.add(controller.set_no_mqtt("mqtt" not in CORE.config))
        if "http_request" in CORE.config:
            cg.add_define("USE_XR806_STANDBY_HTTP_CLIENT")
        cg.add(controller.set_external_sync_required(cfg[CONF_REQUIRE_EXTERNAL_SYNC]))
        cg.add(controller.set_require_uart_participant("uart" in CORE.config))
        cg.add(controller.set_stay_connected(config[CONF_WLAN][CONF_STAY_CONNECTED]))
        await cg.register_component(controller, cfg)
        if has_http:
            # Dedicated HTTP + SSE socket owner holds a physical PM wakelock,
            # closes only the listener during preflight, and waits for clients.
            cg.add_define("USE_XR806_HTTP_STANDBY")
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/http_standby.h"')
            )
            http_standby = cg.new_Pvariable(cfg[CONF_HTTP_STANDBY_ID])
            await cg.register_component(http_standby, {})
        if "api" in CORE.config:
            # Votes API_BUSY from the public APIServer state.
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/api_standby.h"')
            )
            api_standby = cg.new_Pvariable(cfg[CONF_API_STANDBY_ID])
            await cg.register_component(api_standby, {})
            if (offload := cfg.get(CONF_API_KEEPALIVE_OFFLOAD)) is not None:
                cg.add_define("USE_XR806_API_KEEPALIVE_OFFLOAD")
                cg.add(
                    api_standby.set_keepalive_offload(
                        offload[CONF_PERIOD].total_seconds,
                        offload[CONF_HOST_SLEEP],
                        offload[CONF_WAKE_ON_PEER],
                    )
                )
        if "mqtt" in CORE.config:
            # Disconnects cleanly before standby and votes MQTT_BUSY until the socket is closed.
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/mqtt_standby.h"')
            )
            mqtt_standby = cg.new_Pvariable(cfg[CONF_MQTT_STANDBY_ID])
            await cg.register_component(mqtt_standby, {})
        if "ota" in CORE.config:
            # OTA state updates are informative, NOT evidence that the
            # OTA listener and flash writer are already disarmed.
            cg.add_define("USE_OTA_STATE_LISTENER")
            cg.add_global(
                cg.RawStatement(
                    '#include "esphome/components/xr806/standby_ota_guard.h"'
                )
            )
            ota_guard = cg.new_Pvariable(cfg[CONF_OTA_GUARD_ID])
            # HTTP uploads share the listener and preflight with HTTP_BUSY.
            # Native OTA uses an independent TCP listener and remains blocked.
            ota_configs = CORE.config["ota"]
            cg.add(
                ota_guard.set_http_only(
                    bool(ota_configs)
                    and all(
                        item.get("platform") == "web_server" for item in ota_configs
                    )
                )
            )
            await cg.register_component(ota_guard, cfg)

    cg.add_define("USE_XR806")
    # XR806 uses ordinary addressable memory, not Arduino PROGMEM.
    cg.add_define("PROGMEM")
    cg.add_define(ThreadModel.MULTI_ATOMICS)
    cg.add_define("ESPHOME_BOARD", config[CONF_BOARD])
    # Chip name for the API and for the chipFamily of http_request update manifests.
    cg.add_define("ESPHOME_VARIANT", "XR806")

    if "wifi" in CORE.config:
        wlan = config[CONF_WLAN]
        cg.add_define("XR806_WLAN_DTIM", wlan[CONF_DTIM])
        cg.add_define("XR806_WLAN_LISTEN_INTERVAL", wlan[CONF_LISTEN_INTERVAL])
        if CONF_STANDBY in config:
            # Owns NETWORK_BUSY and the native WLAN wakelock; parks the link only
            # with network_suspend.
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/wifi_standby.h"')
            )
            standby_wlan = cg.new_Pvariable(
                wlan[CONF_ID], wlan[CONF_NETWORK_SUSPEND], wlan[CONF_STAY_CONNECTED]
            )
            await cg.register_component(standby_wlan, {})

    tuya_config = CORE.config.get("tuya")
    if tuya_config and tuya_config.get(CONF_PROTOCOL) == "low_power":
        await _tuya_low_power_host(config[CONF_TUYA_HOST_ID], tuya_config)


def compiler_build_flags(build_flags: set[str]) -> list[str]:
    """Compiler flags from cg.add_build_flag(), such as the log level.

    The vendor Makefile does not see PlatformIO build flags, so these are passed to
    the Make adapter. Linker flags do not apply to its compile rules.
    """
    return sorted(
        flag
        for flag in build_flags
        if flag.startswith(("-D", "-W", "-f")) and not flag.startswith("-Wl,")
    )


def run_compile(args: object, config: ConfigType) -> bool:
    """Build an XR806 AWIH image using the XRadio vendor SDK."""
    import shutil
    import subprocess

    from .build_support.dependencies import checkout_revision, resolve_dependencies
    from .build_support.noise_build import prepare_noise_build

    # Locate the ESPHome checkout from this platform module.
    repo_root = Path(__file__).resolve().parents[3]
    build = Path(CORE.build_path)
    source = build / "src"
    xr_build = build / "xr806"
    obj = xr_build / "obj"
    fw_dir = xr_build / "firmware"

    # Resolve pinned Git dependencies from the local cache.
    try:
        sdk_cache, arduinojson_src = resolve_dependencies()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as err:
        raise EsphomeError(f"XR806 dependency resolution failed: {err}") from err

    adapter = repo_root / "esphome/components/xr806/build_support/xr806_esphome.mk"

    if not adapter.is_file():
        raise EsphomeError(f"XR806 Make adapter not found: {adapter}")

    # The vendor Makefiles generate files inside their SDK tree.
    # Never compile directly inside the shared dependency cache.
    sdk_root = xr_build / "sdk-work"
    # The checkout actually used: the pinned one, or XR806_SDK_SOURCE_DIR at its own revision.
    sdk_revision = checkout_revision(sdk_cache)
    revision_file = sdk_root / ".esphome-xr806-sdk-revision"

    if sdk_root.exists():
        if not sdk_root.is_dir() or not revision_file.is_file():
            raise EsphomeError(
                f"XR806 SDK work directory has unknown contents: "
                f"{sdk_root}. Inspect it before removing or rebuilding it."
            )
        if revision_file.read_text().strip() != sdk_revision:
            # A workspace this build created for another SDK revision: create it again.
            print(
                "[XR806] SDK revision changed; recreating the SDK build workspace",
                flush=True,
            )
            shutil.rmtree(sdk_root)
        else:
            print("[XR806] Reusing SDK build workspace", flush=True)
    if not sdk_root.exists():
        import tempfile

        xr_build.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".sdk-stage-", dir=xr_build))

        try:
            print("[XR806] Creating isolated SDK workspace", flush=True)
            shutil.copytree(
                sdk_cache,
                staging,
                dirs_exist_ok=True,
                symlinks=True,
                ignore=shutil.ignore_patterns(".git"),
            )
            (staging / revision_file.name).write_text(sdk_revision + "\n")
            staging.rename(sdk_root)
        except (OSError, shutil.Error) as err:
            raise EsphomeError(f"Could not prepare XR806 SDK workspace: {err}") from err
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    sdk = sdk_root / "project/demo/trv16_port/gcc"

    if not sdk.is_dir():
        raise EsphomeError(f"XR806 vendor Makefile directory missing: {sdk}")

    elf = xr_build / "app.elf"
    image = fw_dir / "firmware.img"

    if not sdk.is_dir() or not adapter.is_file():
        raise EsphomeError("XR806 SDK or Make adapter not found")

    if not (source / "main.cpp").is_file():
        raise EsphomeError("XR806 generated main.cpp is missing")

    variables = {
        "XR806_REPO_ROOT": str(repo_root),
        "XR806_ARDUINOJSON_SRC": str(arduinojson_src),
        "XR806_SRC": str(source),
        "XR806_OBJDIR": str(obj),
        "XR806_ESPHOME_MAIN": str(obj / "sdk_main_esphome.o"),
        "XR806_OUTPUT": str(elf),
        "XR806_FW_DIR": str(fw_dir),
        "XR806_IMAGE_CFG": str(fw_dir / "image.cfg"),
        "XR806_AWIH": str(image),
    }

    import shlex

    extra_flags = shlex.join(compiler_build_flags(CORE.build_flags))
    obj.mkdir(parents=True, exist_ok=True)
    flags_stamp = obj / "esphome_build_flags.txt"
    if not flags_stamp.is_file() or flags_stamp.read_text() != extra_flags:
        flags_stamp.write_text(extra_flags)
    variables["XR806_EXTRA_CXXFLAGS"] = extra_flags
    variables["XR806_FLAGS_STAMP"] = str(flags_stamp)

    # Plain esphome compile owns crypto builds.
    import re

    defines = (source / "esphome/core/defines.h").read_text()
    if re.search(r"(?m)^\s*#define\s+USE_NOISE(?:\s|$)", defines):
        try:
            variables.update(prepare_noise_build())
        except (OSError, RuntimeError, subprocess.CalledProcessError) as err:
            raise EsphomeError(f"XR806 Noise ARM build failed: {err}") from err

    command = [
        "make",
        "-f",
        "Makefile",
        "-f",
        str(adapter),
        *(f"{key}={value}" for key, value in variables.items()),
        str(image),
    ]

    print(f"[XR806] Building: {CORE.name}", flush=True)

    try:
        result = subprocess.run(
            command,
            cwd=sdk,
            check=False,
        )
    except OSError as err:
        raise EsphomeError(f"XR806 build could not start: {err}") from err

    if result.returncode != 0:
        raise EsphomeError(f"XR806 build failed (exit {result.returncode})")

    if not image.is_file():
        raise EsphomeError(f"XR806 firmware missing: {image}")

    with image.open("rb") as firmware:
        if firmware.read(4) != b"AWIH":
            raise EsphomeError("Invalid XR806 AWIH firmware header")

    from .build_support.ota_image import make_ota_image, stamp_boot_header

    try:
        firmware = stamp_boot_header(
            image.read_bytes(),
            Path(variables["XR806_IMAGE_CFG"]).read_text(encoding="utf-8"),
        )
    except (OSError, ValueError) as err:
        raise EsphomeError(f"XR806 BOOT header: {err}") from err

    destination = CORE.firmware_bin
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or destination.read_bytes() != firmware:
        destination.write_bytes(firmware)
    print(f"[XR806] Firmware: {destination}", flush=True)

    # The network update file: the APP sections compressed for the bootloader.
    # firmware.bin stays the full image for a wired flash.
    ota_output = destination.with_name(OTA_IMAGE_NAME)
    ota_output.unlink(missing_ok=True)
    if "ota" in CORE.config:
        try:
            ota, info = make_ota_image(firmware)
        except ValueError as err:
            raise EsphomeError(f"XR806 OTA image: {err}") from err
        ota_output.write_bytes(ota)
        print(
            f"[XR806] OTA image: {ota_output} ({info.ota_bytes} of {info.slot_bytes} bytes)",
            flush=True,
        )

    return True


def ota_image(binary: Path) -> Path:
    """Network updates send the compressed APP image, not the full firmware.bin."""
    ota = Path(binary).with_name(OTA_IMAGE_NAME)
    if not ota.is_file():
        raise EsphomeError(f"No XR806 OTA image was built: {ota}")
    return ota


# Register the platform's native GPIO schema during platform import.
from . import gpio as _xr806_gpio  # noqa: E402, F401  # pylint: disable=wrong-import-position
