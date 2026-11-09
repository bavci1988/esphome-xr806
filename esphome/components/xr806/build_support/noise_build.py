"""Incremental ARM cross-build of ESPHome's pinned noise-c and libsodium."""

from __future__ import annotations

import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from .dependencies import ensure_dependency

BUILD_CACHE = Path.home() / ".cache/esphome-xr806/noise-arm"
ARM_FLAGS = (
    "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=softfp "
    "-mcmse -Os -ffunction-sections -fdata-sections "
    "-DHAVE_WEAK_SYMBOLS=1 -DHAVE_INLINE_ASM=1"
)
CMAKE_PROJECT = """cmake_minimum_required(VERSION 3.16)
project(xr806_noise LANGUAGES C ASM)
set(NOISE_C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NOISE_C_FIND_LIBSODIUM OFF CACHE BOOL "" FORCE)
add_subdirectory("${SODIUM_SOURCE}" "${CMAKE_BINARY_DIR}/sodium")
add_subdirectory("${NOISE_SOURCE}" "${CMAKE_BINARY_DIR}/noise-c")
target_compile_definitions(noise_c PRIVATE
    NOISE_USE_PROTOCOL_NAME_TABLE=0
    NOISE_USE_FALLBACK=0
    NOISE_USE_HFS=0)
"""


def _output(*args: str, cwd: Path | None = None) -> str:
    return subprocess.run(
        args, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


def _tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"Required XR806 Noise build tool not found: {name}")
    return str(Path(path).resolve())


def _patched_sodium(source: Path, target: Path) -> None:
    marker = target / ".xr806-patches-applied"
    if marker.is_file():
        return
    if target.exists():
        raise RuntimeError(f"Incomplete patched libsodium: {target}")
    shutil.copytree(source, target, symlinks=True)
    # The pinned source is never modified. This isolated copy includes
    # the git submodule metadata required by patches/apply.sh.
    subprocess.run(["sh", "patches/apply.sh"], cwd=target, check=True)
    marker.write_text("patched\n")


def prepare_noise_build() -> dict[str, str]:
    """Resolve source pins, cross-build archives and provide Make variables."""
    noise = ensure_dependency("noise_c")
    sodium = ensure_dependency("libsodium")
    cc, ar, ranlib = (
        _tool(x)
        for x in ("arm-none-eabi-gcc", "arm-none-eabi-ar", "arm-none-eabi-ranlib")
    )
    _tool("cmake")
    digest = hashlib.sha256(
        json.dumps(
            {
                "noise": _output("git", "rev-parse", "HEAD", cwd=noise),
                "sodium": _output("git", "rev-parse", "HEAD", cwd=sodium),
                "compiler": cc,
                "compiler_version": _output(cc, "--version").splitlines()[0],
                "ar": ar,
                "ranlib": ranlib,
                "flags": ARM_FLAGS,
                "cmake_project": CMAKE_PROJECT,
                "patches": [
                    (p.name, hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in sorted((sodium / "patches").glob("*.patch"))
                ],
            },
            sort_keys=True,
        ).encode()
    ).hexdigest()[:24]

    if not list((sodium / "patches").glob("*.patch")):
        raise RuntimeError("libsodium port patches missing")

    BUILD_CACHE.mkdir(parents=True, exist_ok=True)
    with (BUILD_CACHE / ".build.lock").open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            work = BUILD_CACHE / digest
            project = work / "project"
            build = work / "build"
            patched = work / "libsodium-patched"
            project.mkdir(parents=True, exist_ok=True)
            _patched_sodium(sodium, patched)

            toolchain = project / "arm.cmake"
            toolchain.write_text(
                "\n".join(
                    (
                        "set(CMAKE_SYSTEM_NAME Generic)",
                        "set(CMAKE_SYSTEM_PROCESSOR arm)",
                        "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)",
                        f'set(CMAKE_C_COMPILER "{cc}")',
                        f'set(CMAKE_ASM_COMPILER "{cc}")',
                        f'set(CMAKE_AR "{ar}")',
                        f'set(CMAKE_RANLIB "{ranlib}")',
                        f'set(CMAKE_C_FLAGS_INIT "{ARM_FLAGS}")',
                        f'set(CMAKE_ASM_FLAGS_INIT "{ARM_FLAGS}")',
                    )
                )
                + "\n"
            )
            (project / "CMakeLists.txt").write_text(CMAKE_PROJECT)

            subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(project),
                    "-B",
                    str(build),
                    "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain),
                    "-DCMAKE_BUILD_TYPE=MinSizeRel",
                    "-DNOISE_SOURCE=" + str(noise),
                    "-DSODIUM_SOURCE=" + str(patched),
                ],
                check=True,
            )
            subprocess.run(
                ["cmake", "--build", str(build), "--parallel", "4"],
                check=True,
            )

            archives = {
                "XR806_NOISE_C_ARCHIVE": build / "noise-c/libnoise_c.a",
                "XR806_SODIUM_ARCHIVE": build / "sodium/libsodium.a",
            }
            for archive in archives.values():
                if not archive.is_file() or archive.stat().st_size < 256:
                    raise RuntimeError(f"Missing ARM archive: {archive}")
                if not _output(ar, "t", str(archive)):
                    raise RuntimeError(f"Empty ARM archive: {archive}")

            print("[XR806] Noise ARM libraries ready (incremental)", flush=True)
            return {
                **{k: str(v) for k, v in archives.items()},
                "XR806_NOISE_C_INCLUDE": str(noise / "include"),
                "XR806_NOISE_SRC_INCLUDE": str(noise / "src"),
                "XR806_SODIUM_INCLUDE": str(
                    patched / "libsodium/src/libsodium/include"
                ),
                "XR806_SODIUM_PORT_INCLUDE": str(patched / "port_include"),
            }
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
