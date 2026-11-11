"""Native ESPHome platform for the Allwinner/XRadio XR806 SoC."""

from pathlib import Path

from esphome import final_validate as fv
import esphome.codegen as cg
from esphome.components.const import CONF_ENABLED
import esphome.config_validation as cv
from esphome.const import (
    CONF_BOARD,
    CONF_ID,
    KEY_CORE,
    KEY_FRAMEWORK_VERSION,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    Framework,
    ThreadModel,
)
from esphome.core import CORE, EsphomeError
from esphome.types import ConfigType

CODEOWNERS = ["@bavci1988"]
# The preferences syncer writes pending preferences every 60 s and at shutdown.
AUTO_LOAD = ["preferences"]
DOMAIN = "xr806"
IS_TARGET_PLATFORM = True

OTA_IMAGE_NAME = "firmware.ota.bin"

CONF_WLAN = "wlan"
CONF_NETWORK_SUSPEND = "network_suspend"
CONF_DTIM = "dtim"
CONF_LISTEN_INTERVAL = "listen_interval"
CONF_STANDBY_TRIAL = "standby_trial"
CONF_STANDBY = "standby"
CONF_OTA_GUARD_ID = "ota_guard_id"
CONF_OTA_DIAGNOSTIC_READONLY = "ota_diagnostic_readonly"
CONF_API_STANDBY_ID = "api_standby_id"
CONF_HTTP_STANDBY_ID = "http_standby_id"
CONF_MQTT_STANDBY_ID = "mqtt_standby_id"
CONF_QUIET_MS = "quiet_ms"
CONF_MIN_AWAKE_MS = "min_awake_ms"
CONF_REQUIRE_EXTERNAL_SYNC = "require_external_sync"
CONF_ENTRY_AFTER_MS = "entry_after_ms"
CONF_WAKE_AFTER_MS = "wake_after_ms"
CONF_WAKE_PIN = "wake_pin"
CONF_WAKE_EDGE = "wake_edge"
CONF_RESUME_MARKER_PB05 = "resume_marker_pb05"

xr806_ns = cg.esphome_ns.namespace("xr806")
XR806WiFiStandby = xr806_ns.class_("XR806WiFiStandby", cg.Component)
XR806APIStandby = xr806_ns.class_("XR806APIStandby", cg.Component)
XR806StandbyTrial = xr806_ns.class_("XR806StandbyTrial", cg.Component)
XR806StandbyController = xr806_ns.class_("XR806StandbyController", cg.Component)
XR806OTAStandbyGuard = xr806_ns.class_("XR806OTAStandbyGuard", cg.Component)
XR806HTTPStandby = xr806_ns.class_("XR806HTTPStandby", cg.Component)
XR806MQTTStandby = xr806_ns.class_("XR806MQTTStandby", cg.Component)


# Radio settings for the standard wifi: component. DTIM and the listen interval
# apply when wifi: power_save_mode is LIGHT or HIGH.
XR806_WLAN_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XR806WiFiStandby),
        cv.Optional(CONF_DTIM, default=1): cv.int_range(min=1, max=30),
        cv.Optional(CONF_LISTEN_INTERVAL, default=0): cv.int_range(min=0, max=30),
        cv.Optional(CONF_NETWORK_SUSPEND, default=False): cv.boolean,
    }
)


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
        cv.GenerateID(CONF_API_STANDBY_ID): cv.declare_id(XR806APIStandby),
        cv.GenerateID(CONF_HTTP_STANDBY_ID): cv.declare_id(XR806HTTPStandby),
        cv.GenerateID(CONF_MQTT_STANDBY_ID): cv.declare_id(XR806MQTTStandby),
        cv.Required(CONF_ENABLED): cv.boolean,
        cv.Optional(CONF_WAKE_AFTER_MS, default=3000): cv.int_range(min=500, max=60000),
        cv.Optional(CONF_QUIET_MS, default=250): cv.int_range(min=200, max=30000),
        cv.Optional(CONF_MIN_AWAKE_MS, default=5000): cv.int_range(min=1000, max=60000),
        cv.Optional(CONF_WAKE_PIN): cv.one_of(
            "PA11", "PA12", "PA13", "PA19", "PA20", upper=True
        ),
        cv.Optional(CONF_WAKE_EDGE): cv.one_of("falling", "rising", lower=True),
        cv.Optional(CONF_REQUIRE_EXTERNAL_SYNC, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _update_core_data(config: ConfigType) -> None:
    if config[CONF_WLAN][CONF_NETWORK_SUSPEND] and CONF_STANDBY not in config:
        raise cv.Invalid(
            "xr806.wlan.network_suspend requires xr806.standby and full PM gating"
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
    CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = DOMAIN
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = Framework.XRADIO
    # Version of the pinned XRadio SDK (include/version.h), for require_framework_version().
    CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION] = cv.Version(1, 2, 2)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional("lowpower", default=False): cv.boolean,
            cv.Required(CONF_BOARD): cv.one_of("wxu", lower=True),
            cv.Optional(CONF_WLAN, default={}): XR806_WLAN_SCHEMA,
            cv.Optional(CONF_STANDBY_TRIAL): STANDBY_TRIAL_SCHEMA,
            cv.Optional(CONF_STANDBY): STANDBY_SCHEMA,
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


FINAL_VALIDATE_SCHEMA = _final_validate_uart


async def to_code(config: ConfigType) -> None:
    # The FDCM journal finds stored preferences by key.
    cg.add_define("USE_PREFERENCE_KEY_LOOKUP")
    if config[CONF_OTA_DIAGNOSTIC_READONLY]:
        # ROM/FDCM read-only snapshot: does not enable ESPHome OTA.
        cg.add_define("USE_XR806_OTA_RUNTIME_PROBE")
    # Activate future XR806 PM policy
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
            standby_wlan = cg.new_Pvariable(wlan[CONF_ID], wlan[CONF_NETWORK_SUSPEND])
            await cg.register_component(standby_wlan, {})


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
