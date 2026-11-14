"""A local XR806 SDK checkout for SDK development and CI.

Builds normally use the SDK release archive that framework.py installs.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess


def _git(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


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
