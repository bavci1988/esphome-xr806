"""Native ESPHome platform for the Allwinner/XRadio XR806 SoC."""

import esphome.codegen as cg
import esphome.config_validation as cv

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


def _update_core_data(config):
    CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = DOMAIN
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "xradio"
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            # XR806_LOWPOWER_FLAG_V1
            cv.Optional("lowpower", default=False): cv.boolean,
            cv.Required(CONF_BOARD): cv.one_of("wxu", lower=True),
        }
    ),
    _update_core_data,
)


async def to_code(config):
    # XR806_LOWPOWER_FLAG_V1: activate future XR806 PM policy
    if config["lowpower"]:
        cg.add_define("USE_XR806_LOWPOWER")
    cg.add_define("USE_XR806")
    # XR806 uses ordinary addressable memory, not Arduino PROGMEM.
    cg.add_define("PROGMEM")
    cg.add_define(ThreadModel.MULTI_ATOMICS)
    cg.add_define("ESPHOME_BOARD", config[CONF_BOARD])


def run_compile(args, config) -> bool:
    """Build an XR806 AWIH image using the XRadio vendor SDK."""
    import filecmp
    import shutil
    import subprocess
    from pathlib import Path

    from esphome.core import EsphomeError
    from .build_support.dependencies import DEPENDENCIES, resolve_dependencies

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
        raise EsphomeError(
            f"XR806 dependency resolution failed: {err}"
        ) from err

    adapter = (
        repo_root
        / "esphome/components/xr806/build_support/xr806_esphome.mk"
    )

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
        staging = Path(
            tempfile.mkdtemp(prefix=".sdk-stage-", dir=xr_build)
        )

        try:
            print("[XR806] Creating isolated SDK workspace", flush=True)
            shutil.copytree(
                sdk_cache,
                staging,
                dirs_exist_ok=True,
                symlinks=True,
                ignore=shutil.ignore_patterns(".git"),
            )
            (staging / revision_file.name).write_text(
                sdk_revision + "\n"
            )
            staging.rename(sdk_root)
        except (OSError, shutil.Error) as err:
            raise EsphomeError(
                f"Could not prepare XR806 SDK workspace: {err}"
            ) from err
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    sdk = sdk_root / "project/demo/trv16_port/gcc"

    if not sdk.is_dir():
        raise EsphomeError(
            f"XR806 vendor Makefile directory missing: {sdk}"
        )

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

    command = [
        "make",
        "-f", "Makefile",
        "-f", str(adapter),
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
        raise EsphomeError(
            f"XR806 build failed (exit {result.returncode})"
        )

    if not image.is_file():
        raise EsphomeError(f"XR806 firmware missing: {image}")

    with image.open("rb") as firmware:
        if firmware.read(4) != b"AWIH":
            raise EsphomeError("Invalid XR806 AWIH firmware header")

    destination = CORE.firmware_bin
    destination.parent.mkdir(parents=True, exist_ok=True)

    if not destination.is_file() or not filecmp.cmp(
        image, destination, shallow=False
    ):
        shutil.copy2(image, destination)

    print(f"[XR806] AWIH firmware: {image}", flush=True)
    print(f"[XR806] ESPHome firmware: {destination}", flush=True)

    return True


# XR806_GPIO_REGISTRATION_V1
# Register the platform's native GPIO schema during platform import.
from . import gpio as _xr806_gpio  # noqa: F401
