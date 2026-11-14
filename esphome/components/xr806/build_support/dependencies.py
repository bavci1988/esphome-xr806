"""Pinned source dependencies of the XR806 build, and a local SDK checkout for development.

The SDK and the compiler come from framework.py as release archives."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from esphome.framework_helpers import install_lock

CACHE_ROOT = Path.home() / ".cache/esphome-xr806/dependencies"

DEPENDENCIES = {
    "arduinojson": {
        "url": "https://github.com/bblanchon/ArduinoJson.git",
        "commit": "77771d3c07668e01d8f52acb03910c1110bb373f",
        "required": (
            "src/ArduinoJson.h",
            "LICENSE.txt",
        ),
    },
    # Version-pinned native ESPHome API Noise crypto dependencies.
    "noise_c": {
        "url": "https://github.com/esphome-libs/noise-c.git",
        "commit": "62c2fdca7a874ea898761147bb062ca1cde9f36d",  # v0.1.31
        "required": ("CMakeLists.txt", "include/noise/protocol.h"),
    },
    "libsodium": {
        "url": "https://github.com/esphome-libs/libsodium.git",
        "commit": "95c5e60d991b3f735619da8a307951c0483ef567",  # 1.10021.12
        "submodules": True,
        "required": (
            "CMakeLists.txt",
            "patches/apply.sh",
            "libsodium/src/libsodium/include/sodium.h",
        ),
    },
}


def _git(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def checkout_revision(path: Path) -> str:
    """Return the Git revision of a verified dependency checkout."""
    return _git("rev-parse", "HEAD", cwd=path)


def _verify(path: Path, name: str, expected_commit: str | None = None) -> None:
    """Verify the cached Git revision and required files."""
    info = DEPENDENCIES[name]

    if not (path / ".git").exists():
        raise RuntimeError(f"Invalid dependency cache (missing Git metadata): {path}")

    revision = _git("rev-parse", "HEAD", cwd=path)

    if revision != (expected_commit or info["commit"]):
        raise RuntimeError(f"Unexpected {name} revision: {revision}")

    changes = _git(
        "status",
        "--porcelain",
        "--untracked-files=all",
        cwd=path,
    )

    if changes:
        raise RuntimeError(f"Cached {name} sources have local modifications: {path}")

    for relative in info["required"]:
        if not (path / relative).is_file():
            raise RuntimeError(f"Missing {name} dependency file: {relative}")


# Files every SDK checkout needs, like the release archive's layout check
_SDK_REQUIRED = (
    "gcc.mk",
    ".config",
    "project/common/board/xr806_wxu/board_config.c",
    "project/demo/trv16_port/gcc/Makefile",
    "lib/xradio_v3/libwlan.a",
    "bin/xradio_v3/wlan_fw.bin",
)


def sdk_source_override() -> tuple[Path, str] | None:
    """A local SDK checkout from XR806_SDK_SOURCE_DIR, with its Git revision.

    For SDK development and CI. The checkout must be clean;
    XR806_SDK_EXPECTED_COMMIT, when set, must equal its revision.
    """
    override = os.environ.get("XR806_SDK_SOURCE_DIR", "").strip()
    if not override:
        return None
    source = Path(override).expanduser().resolve(strict=True)
    if not (source / ".git").exists():
        raise RuntimeError(f"XR806_SDK_SOURCE_DIR is not a Git checkout: {source}")
    revision = _git("rev-parse", "HEAD", cwd=source)
    expected = os.environ.get("XR806_SDK_EXPECTED_COMMIT", "").strip()
    if expected and (
        len(expected) != 40 or any(c not in "0123456789abcdef" for c in expected)
    ):
        raise RuntimeError(
            "Invalid XR806_SDK_EXPECTED_COMMIT: expected full lowercase Git SHA"
        )
    if expected and revision != expected:
        raise RuntimeError(f"Unexpected xr806_sdk revision: {revision}")
    if _git("status", "--porcelain", "--untracked-files=all", cwd=source):
        raise RuntimeError(
            f"Cached xr806_sdk sources have local modifications: {source}"
        )
    for relative in _SDK_REQUIRED:
        if not (source / relative).is_file():
            raise RuntimeError(f"Missing xr806_sdk dependency file: {relative}")
    print(f"[XR806] Verified external SDK checkout: {source}", flush=True)
    return source, revision


def ensure_dependency(name: str) -> Path:
    """Ensure a pinned dependency is present in the shared cache."""
    if name not in DEPENDENCIES:
        raise ValueError(f"Unknown XR806 dependency: {name}")

    info = DEPENDENCIES[name]

    parent = CACHE_ROOT / name
    destination = parent / info["commit"]

    parent.mkdir(parents=True, exist_ok=True)

    # Coordinates independent build processes that fetch the same dependency.
    with install_lock(parent, ".download"):
        # Check again after obtaining the lock: another process
        # might have completed the download while we waited.
        if destination.exists() or destination.is_symlink():
            try:
                _verify(destination, name)
            except (
                OSError,
                RuntimeError,
                subprocess.CalledProcessError,
            ) as err:
                raise RuntimeError(
                    f"Invalid XR806 dependency cache: {destination}. "
                    "Move the damaged directory aside before retrying."
                ) from err

            print(f"[XR806] Cache hit: {name}", flush=True)
            return destination

        temporary = Path(
            tempfile.mkdtemp(
                prefix=f".{name}-",
                dir=parent,
            )
        )

        try:
            print(f"[XR806] Downloading: {name}", flush=True)

            subprocess.run(
                [
                    "git",
                    "clone",
                    "--quiet",
                    "--no-checkout",
                    info["url"],
                    str(temporary),
                ],
                check=True,
            )

            _git(
                "checkout",
                "--detach",
                "--quiet",
                info["commit"],
                cwd=temporary,
            )

            if info.get("submodules"):
                subprocess.run(
                    ["git", "submodule", "update", "--init", "--recursive"],
                    cwd=temporary,
                    check=True,
                )

            _verify(temporary, name)

            # Atomic publication of a fully verified checkout.
            temporary.rename(destination)

        finally:
            if temporary.exists():
                shutil.rmtree(temporary)

        print(f"[XR806] Cached: {name}", flush=True)
        return destination


def resolve_dependencies() -> Path:
    """Return the verified ArduinoJson source directory."""
    return ensure_dependency("arduinojson") / "src"
