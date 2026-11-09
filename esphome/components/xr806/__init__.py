"""Native ESPHome platform for the Allwinner/XRadio XR806 SoC."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.const import CONF_ENABLED
from esphome.const import (
    CONF_BOARD,
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    ThreadModel,
)
from esphome.core import CORE

CODEOWNERS = []
DOMAIN = "xr806"
IS_TARGET_PLATFORM = True

# Temporary vendor WLAN smoke probe, not the standard ESPHome wifi: component.
CONF_WLAN_SMOKE = "wlan_smoke"
CONF_XR806_WIFI = "wifi"
CONF_XR806_WLAN_SETTINGS = "wlan"
CONF_NETWORK_BRIDGE = "network_bridge"
CONF_NETWORK_SUSPEND = "network_suspend"
CONF_DTIM = "dtim"
CONF_LISTEN_INTERVAL = "listen_interval"
CONF_POWER_SAVE = "power_save"
CONF_SOCKET_PROBE = "socket_probe"
CONF_SOCKET_FACADE_PROBE = "socket_facade_probe"
CONF_STANDBY_TRIAL = "standby_trial"
CONF_STANDBY = "standby"
CONF_OTA_GUARD_ID = "ota_guard_id"
CONF_QUIET_MS = "quiet_ms"
CONF_MIN_AWAKE_MS = "min_awake_ms"
CONF_REQUIRE_EXTERNAL_SYNC = "require_external_sync"
CONF_ENTRY_AFTER_MS = "entry_after_ms"
CONF_WAKE_AFTER_MS = "wake_after_ms"
CONF_WAKE_PIN = "wake_pin"
CONF_WAKE_EDGE = "wake_edge"
CONF_RESUME_MARKER_PB05 = "resume_marker_pb05"

xr806_ns = cg.esphome_ns.namespace("xr806")
XR806WLANSmoke = xr806_ns.class_("XR806WLANSmoke", cg.Component)
XR806WiFiComponent = xr806_ns.class_("XR806WiFiComponent", cg.Component)
XR806StandbyTrial = xr806_ns.class_("XR806StandbyTrial", cg.Component)
XR806StandbyController = xr806_ns.class_("XR806StandbyController", cg.Component)
XR806OTAStandbyGuard = xr806_ns.class_("XR806OTAStandbyGuard", cg.Component)


def validate_wlan_smoke_password(value):
    value = cv.string_strict(value)
    if value and not 8 <= len(value) <= 63:
        raise cv.Invalid("WLAN smoke password must be empty or 8..63 characters")
    return value


WLAN_SMOKE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806WLANSmoke),
        cv.Required("ssid"): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
        cv.Required("password"): validate_wlan_smoke_password,
        cv.Optional(CONF_POWER_SAVE, default=False): cv.boolean,
        cv.Optional(CONF_SOCKET_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_SOCKET_FACADE_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_DTIM, default=1): cv.int_range(min=1, max=30),
        cv.Optional(CONF_LISTEN_INTERVAL, default=0): cv.int_range(min=0, max=30),
    }
).extend(cv.COMPONENT_SCHEMA)

# Production-name schema; same SDK options as legacy probe during migration.
XR806_WLAN_SETTINGS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_POWER_SAVE, default=False): cv.boolean,
        cv.Optional(CONF_DTIM, default=1): cv.int_range(min=1, max=30),
        cv.Optional(CONF_LISTEN_INTERVAL, default=0): cv.int_range(min=0, max=30),
        cv.Optional(CONF_SOCKET_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_SOCKET_FACADE_PROBE, default=False): cv.boolean,
    }
)

XR806_WIFI_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806WiFiComponent),
        cv.Required("ssid"): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
        cv.Required("password"): validate_wlan_smoke_password,
        cv.Optional(CONF_NETWORK_BRIDGE, default=False): cv.boolean,
        cv.Optional(CONF_NETWORK_SUSPEND, default=False): cv.boolean,
        cv.Optional(CONF_POWER_SAVE, default=False): cv.boolean,
        cv.Optional(CONF_SOCKET_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_SOCKET_FACADE_PROBE, default=False): cv.boolean,
        cv.Optional(CONF_DTIM, default=1): cv.int_range(min=1, max=30),
        cv.Optional(CONF_LISTEN_INTERVAL, default=0): cv.int_range(min=0, max=30),
    }
).extend(cv.COMPONENT_SCHEMA)


# Stage 7b is an OFFLINE, bounded one-shot for an isolated WXU test board.
# Exposed PA wake pins only; PA21..23 are reserved for UART2/boot.
STANDBY_TRIAL_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806StandbyTrial),
        cv.Required("confirm_offline"): cv.boolean,
        cv.Optional(CONF_ENTRY_AFTER_MS, default=10000): cv.int_range(
            min=1000, max=60000
        ),
        cv.Required(CONF_WAKE_AFTER_MS): cv.int_range(min=100, max=5000),
        cv.Optional(CONF_WAKE_PIN): cv.one_of(
            "PA11", "PA12", "PA13", "PA19", "PA20", upper=True
        ),
        cv.Optional(CONF_WAKE_EDGE, default="falling"): cv.one_of(
            "falling", "rising", lower=True
        ),
        cv.Optional(CONF_RESUME_MARKER_PB05, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


# Stage 7b production-side coordinator: deliberately does NOT implement any
# Tuya/HC32 commands, UART autobaud, or transport-specific idle voting.
# A firmware with active transports must register lifecycle participants
# and its owners must actually clear their fail-closed PM blockers.
STANDBY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806StandbyController),
        cv.GenerateID(CONF_OTA_GUARD_ID): cv.declare_id(XR806OTAStandbyGuard),
        cv.Required(CONF_ENABLED): cv.boolean,
        cv.Optional(CONF_WAKE_AFTER_MS, default=3000): cv.int_range(
            min=500, max=60000
        ),
        cv.Optional(CONF_QUIET_MS, default=250): cv.int_range(
            min=200, max=30000
        ),
        cv.Optional(CONF_MIN_AWAKE_MS, default=5000): cv.int_range(
            min=1000, max=60000
        ),
        cv.Optional(CONF_WAKE_PIN): cv.one_of(
            "PA11", "PA12", "PA13", "PA19", "PA20", upper=True
        ),
        cv.Optional(CONF_WAKE_EDGE): cv.one_of(
            "falling", "rising", lower=True
        ),
        cv.Optional(
            CONF_REQUIRE_EXTERNAL_SYNC, default=False
        ): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _update_core_data(config):
    if CONF_XR806_WLAN_SETTINGS in config and (
        CONF_XR806_WIFI in config or CONF_WLAN_SMOKE in config
    ):
        raise cv.Invalid("xr806.wlan cannot be combined with xr806.wifi/wlan_smoke")
    if CONF_WLAN_SMOKE in config and CONF_XR806_WIFI in config:
        raise cv.Invalid(
            "Choose either xr806.wifi or legacy xr806.wlan_smoke, not both"
        )
    if CONF_XR806_WIFI in config and config[CONF_XR806_WIFI][CONF_NETWORK_SUSPEND]:
        if CONF_STANDBY not in config:
            raise cv.Invalid(
                "xr806.wifi.network_suspend requires xr806.standby and full PM gating"
            )
        if config[CONF_XR806_WIFI][CONF_SOCKET_PROBE] or config[CONF_XR806_WIFI][CONF_SOCKET_FACADE_PROBE]:
            raise cv.Invalid(
                "xr806.wifi.network_suspend cannot share ad-hoc socket probes"
            )
    if CONF_STANDBY in config:
        standby_cfg = config[CONF_STANDBY]
        if (CONF_WAKE_PIN in standby_cfg) != (CONF_WAKE_EDGE in standby_cfg):
            raise cv.Invalid(
                "xr806.standby requires explicit wake_pin and wake_edge together"
            )
        if not config["lowpower"]:
            raise cv.Invalid("xr806.standby requires xr806.lowpower: true")
        if not config[CONF_STANDBY][CONF_ENABLED]:
            raise cv.Invalid("Remove xr806.standby when disabled")
        if CONF_STANDBY_TRIAL in config:
            raise cv.Invalid("Choose standby or standby_trial, not both")
    if CONF_STANDBY_TRIAL in config:
        trial = config[CONF_STANDBY_TRIAL]
        if not config["lowpower"]:
            raise cv.Invalid("xr806.standby_trial requires xr806.lowpower: true")
        if not trial["confirm_offline"]:
            raise cv.Invalid("xr806.standby_trial requires confirm_offline: true")
        # PB05 is in the WXU board's conservative Flash/XIP reservation.
        # No electrical pin-ownership proof exists for the live TRV16.
        if trial[CONF_RESUME_MARKER_PB05]:
            raise cv.Invalid(
                "resume_marker_pb05 is unsafe on the current WXU pin map; "
                "use timer/current measurements instead"
            )
        if (
            CONF_WLAN_SMOKE in config
            or CONF_XR806_WIFI in config
            or CONF_XR806_WLAN_SETTINGS in config
        ):
            raise cv.Invalid(
                "Stage 7b offline standby_trial cannot be combined with XR806 WLAN"
            )
    CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = DOMAIN
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "xradio"
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            # XR806_LOWPOWER_FLAG_V1
            cv.Optional("lowpower", default=False): cv.boolean,
            cv.Required(CONF_BOARD): cv.one_of("wxu", lower=True),
            cv.Optional(CONF_WLAN_SMOKE): WLAN_SMOKE_SCHEMA,
            cv.Optional(CONF_XR806_WIFI): XR806_WIFI_SCHEMA,
            cv.Optional(CONF_XR806_WLAN_SETTINGS): XR806_WLAN_SETTINGS_SCHEMA,
            cv.Optional(CONF_STANDBY_TRIAL): STANDBY_TRIAL_SCHEMA,
            cv.Optional(CONF_STANDBY): STANDBY_SCHEMA,
        }
    ),
    _update_core_data,
)


async def to_code(config):
    # XR806_LOWPOWER_FLAG_V1: activate future XR806 PM policy
    if config["lowpower"]:
        cg.add_define("USE_XR806_LOWPOWER")
    if CONF_STANDBY in config:
        cfg = config[CONF_STANDBY]
        cg.add_define("USE_XR806_STANDBY_CONTROLLER")
        cg.add_global(
            cg.RawStatement('#include "esphome/components/xr806/standby_controller.h"')
        )
        controller = cg.new_Pvariable(cfg["id"])
        cg.add(controller.set_wake_after_ms(cfg[CONF_WAKE_AFTER_MS]))
        cg.add(controller.set_quiet_ms(cfg[CONF_QUIET_MS]))
        cg.add(controller.set_min_awake_ms(cfg[CONF_MIN_AWAKE_MS]))
        if CONF_WAKE_PIN in cfg:
            cg.add(controller.set_wake_pin(int(cfg[CONF_WAKE_PIN][2:])))
        cg.add(controller.set_wake_falling(cfg.get(CONF_WAKE_EDGE, "falling") == "falling"))
        # Transports which are absent can never have work in flight. Present
        # transports retain their initial busy vote until their own driver
        # explicitly votes idle; no hardcoded MCU or Tuya protocol assumption.
        cg.add(controller.set_no_uart("uart" not in CORE.config))
        wlan = (
            "wifi" in CORE.config
            or "network" in CORE.config
            or CONF_XR806_WIFI in config
            or CONF_WLAN_SMOKE in config
            or CONF_XR806_WLAN_SETTINGS in config
        )
        cg.add(controller.set_no_network(not wlan))
        cg.add(controller.set_no_api("api" not in CORE.config))
        cg.add(controller.set_no_ota("ota" not in CORE.config))
        cg.add(controller.set_no_mdns("mdns" not in CORE.config))
        cg.add(
            controller.set_external_sync_required(
                cfg[CONF_REQUIRE_EXTERNAL_SYNC]
            )
        )
        cg.add(controller.set_require_uart_participant("uart" in CORE.config))
        await cg.register_component(controller, cfg)
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
            await cg.register_component(ota_guard, cfg)

    if CONF_STANDBY_TRIAL in config:
        forbidden = (
            "wifi",
            "api",
            "ota",
            "mqtt",
            "mdns",
            "network",
            "web_server",
            "uart",
            "logger",
            "captive_portal",
            "improv_serial",
        )
        present = [domain for domain in forbidden if domain in CORE.config]
        if present:
            raise cv.Invalid(
                "Stage 7b offline standby_trial forbids active transports: "
                + ", ".join(present)
            )
        trial = config[CONF_STANDBY_TRIAL]
        cg.add_define("USE_XR806_STANDBY_TRIAL")
        cg.add_global(
            cg.RawStatement('#include "esphome/components/xr806/standby_trial.h"')
        )
        standby = cg.new_Pvariable(trial["id"])
        cg.add(standby.set_entry_delay_ms(trial[CONF_ENTRY_AFTER_MS]))
        cg.add(standby.set_wake_after_ms(trial[CONF_WAKE_AFTER_MS]))
        if CONF_WAKE_PIN in trial:
            cg.add(standby.set_wake_pin(int(trial[CONF_WAKE_PIN][2:])))
        cg.add(standby.set_wake_falling(trial[CONF_WAKE_EDGE] == "falling"))
        cg.add(standby.set_resume_marker_pb05(trial[CONF_RESUME_MARKER_PB05]))
        await cg.register_component(standby, trial)
    if CONF_XR806_WLAN_SETTINGS in config and "wifi" not in CORE.config:
        raise cv.Invalid("xr806.wlan requires top-level wifi:")
    cg.add_define("USE_XR806")
    # XR806 uses ordinary addressable memory, not Arduino PROGMEM.
    cg.add_define("PROGMEM")
    cg.add_define(ThreadModel.MULTI_ATOMICS)
    cg.add_define("ESPHOME_BOARD", config[CONF_BOARD])

    if CONF_WLAN_SMOKE in config or CONF_XR806_WIFI in config:
        new_wifi = CONF_XR806_WIFI in config
        wlan_config = config[CONF_XR806_WIFI if new_wifi else CONF_WLAN_SMOKE]
        cg.add_define("USE_XR806_WLAN_SMOKE")
        if new_wifi and wlan_config[CONF_NETWORK_BRIDGE]:
            if "network" not in CORE.config:
                raise cv.Invalid("xr806.wifi.network_bridge requires network:")
            cg.add_define("USE_XR806_NETWORK_BRIDGE")
        if new_wifi:
            cg.add_define("USE_XR806_WIFI")
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/wifi_component.h"')
            )
        else:
            cg.add_global(
                cg.RawStatement('#include "esphome/components/xr806/wlan_smoke.h"')
            )
        wlan = cg.new_Pvariable(wlan_config["id"])
        cg.add(wlan.set_ssid(wlan_config["ssid"]))
        cg.add(wlan.set_password(wlan_config["password"]))
        cg.add(wlan.set_power_save(wlan_config[CONF_POWER_SAVE]))
        if new_wifi and CONF_STANDBY in config:
            cg.add(wlan.set_network_suspend(wlan_config[CONF_NETWORK_SUSPEND]))
        cg.add(wlan.set_socket_probe(wlan_config[CONF_SOCKET_PROBE]))
        cg.add(wlan.set_socket_facade_probe(wlan_config[CONF_SOCKET_FACADE_PROBE]))
        if wlan_config[CONF_SOCKET_FACADE_PROBE]:
            cg.add_define("USE_XR806_SOCKET_FACADE_PROBE")
            cg.add_define("USE_SOCKET_IMPL_LWIP_SOCKETS")
            cg.add_define("USE_NETWORK_IPV4", 1)
        cg.add(wlan.set_dtim(wlan_config[CONF_DTIM]))
        cg.add(wlan.set_listen_interval(wlan_config[CONF_LISTEN_INTERVAL]))
        await cg.register_component(wlan, wlan_config)


def run_compile(args, config) -> bool:
    """Build an XR806 AWIH image using the XRadio vendor SDK."""
    import filecmp
    from pathlib import Path
    import shutil
    import subprocess

    from esphome.core import EsphomeError

    from .build_support.dependencies import DEPENDENCIES, resolve_dependencies
    from .build_support.noise_build import prepare_noise_build

    # Locate the ESPHome checkout from this platform module.
    repo_root = Path(__file__).resolve().parents[3]
    build = Path(CORE.build_path)
    source = build / "src"
    xr_build = build / "xr806"
    obj = xr_build / "obj"
    fw_dir = xr_build / "firmware"

    # XR806_AUTO_DEPENDENCIES_V1
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
    sdk_revision = DEPENDENCIES["xr806_sdk"]["commit"]
    revision_file = sdk_root / ".esphome-xr806-sdk-revision"

    if sdk_root.exists():
        if (
            not sdk_root.is_dir()
            or not revision_file.is_file()
            or revision_file.read_text().strip() != sdk_revision
        ):
            raise EsphomeError(
                f"XR806 SDK work directory has an unexpected revision: "
                f"{sdk_root}. Inspect it before removing or rebuilding it."
            )
        print("[XR806] Reusing SDK build workspace", flush=True)
    else:
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
        "XR806_SKIP_CODEGEN": "1",
        "XR806_YAML": str(CORE.config_path),
        "XR806_SRC": str(source),
        "XR806_OBJDIR": str(obj),
        "XR806_ESPHOME_MAIN": str(obj / "sdk_main_esphome.o"),
        "XR806_OUTPUT": str(elf),
        "XR806_FW_DIR": str(fw_dir),
        "XR806_IMAGE_CFG": str(fw_dir / "image.cfg"),
        "XR806_AWIH": str(image),
    }

    # XR806_NOISE_INTEGRATED_V1: plain esphome compile owns crypto builds.
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

    destination = CORE.firmware_bin
    destination.parent.mkdir(parents=True, exist_ok=True)

    if not destination.is_file() or not filecmp.cmp(image, destination, shallow=False):
        shutil.copy2(image, destination)

    print(f"[XR806] AWIH firmware: {image}", flush=True)
    print(f"[XR806] ESPHome firmware: {destination}", flush=True)

    return True


# XR806_GPIO_REGISTRATION_V1
# Register the platform's native GPIO schema during platform import.
from . import gpio as _xr806_gpio  # noqa: F401  # pylint: disable=wrong-import-position
