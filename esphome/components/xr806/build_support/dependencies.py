"""Pinned external dependencies for the native ESPHome XR806 platform."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path


CACHE_ROOT = Path.home() / ".cache/esphome-xr806/dependencies"

DEPENDENCIES = {
    "xr806_sdk": {
        "url": "git@github.com:bavci1988/xr806-sdk.git",
        "commit": "daeb7f7a6c157a7fe83d59949bbbf4a4cc58616a",
        "required": (
            "gcc.mk",
            ".config",
            "project/common/board/xr806_wxu/board_config.c",
            "project/demo/trv16_port/gcc/Makefile",
            "lib/xradio_v3/libwlan.a",
            "bin/xradio_v3/wlan_fw.bin",
        ),
    },
    "arduinojson": {
        "url": "https://github.com/bblanchon/ArduinoJson.git",
        "commit": "77771d3c07668e01d8f52acb03910c1110bb373f",
        "required": (
            "src/ArduinoJson.h",
            "LICENSE.txt",
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


def _verify(path: Path, name: str) -> None:
    """Verify the cached Git revision and required files."""
    info = DEPENDENCIES[name]

    if not (path / ".git").exists():
        raise RuntimeError(
            f"Invalid dependency cache (missing Git metadata): {path}"
        )

    revision = _git("rev-parse", "HEAD", cwd=path)

    if revision != info["commit"]:
        raise RuntimeError(
            f"Unexpected {name} revision: {revision}"
        )

    changes = _git(
        "status",
        "--porcelain",
        "--untracked-files=no",
        cwd=path,
    )

    if changes:
        raise RuntimeError(
            f"Cached {name} sources have local modifications: {path}"
        )

    for relative in info["required"]:
        if not (path / relative).is_file():
            raise RuntimeError(
                f"Missing {name} dependency file: {relative}"
            )


def ensure_dependency(name: str) -> Path:
    """Download a pinned dependency only when absent from the cache."""
    if name not in DEPENDENCIES:
        raise ValueError(f"Unknown XR806 dependency: {name}")

    info = DEPENDENCIES[name]

    destination = CACHE_ROOT / name / info["commit"]

    if destination.exists():
        _verify(destination, name)
        print(f"[XR806] Cache hit: {name}")
        return destination

    destination.parent.mkdir(parents=True, exist_ok=True)

    temporary = Path(
        tempfile.mkdtemp(
            prefix=f".{name}-",
            dir=destination.parent,
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

        _verify(temporary, name)

        temporary.rename(destination)

    finally:
        if temporary.exists():
            shutil.rmtree(temporary)

    print(f"[XR806] Cached: {name}", flush=True)

    return destination


def resolve_dependencies() -> tuple[Path, Path]:
    """Return the verified SDK and ArduinoJson source directories."""
    sdk = ensure_dependency("xr806_sdk")
    arduinojson = ensure_dependency("arduinojson")

    return sdk, arduinojson / "src"


if __name__ == "__main__":
    sdk, arduinojson = resolve_dependencies()

    print()
    print("XR806 SDK:", sdk)
    print("ArduinoJson:", arduinojson)
    print("All dependencies verified.")
