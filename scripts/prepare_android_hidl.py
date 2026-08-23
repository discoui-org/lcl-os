#!/usr/bin/env python3
"""Prepare official VNDK headers and device HIDL libraries for an ARM64 build."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess


BRANCH = "android12-qpr3-release"
DEVICE_LIBS = (
    "libbase.so", "libcutils.so", "libfmq.so", "libhidlbase.so", "liblog.so",
    "libsync.so", "libutils.so", "libc++.so",
    "android.hardware.graphics.common@1.0.so",
    "android.hardware.graphics.common@1.1.so",
    "android.hardware.graphics.common@1.2.so",
    "android.hardware.graphics.composer@2.1.so",
    "android.hardware.graphics.composer@2.2.so",
    "android.hardware.graphics.composer@2.3.so",
    "android.hardware.graphics.composer@2.4.so",
)


def run(*args: str, cwd: Path | None = None, capture: bool = False) -> str:
    completed = subprocess.run(
        args, cwd=cwd, check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return completed.stdout.strip() if capture else ""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/android-hidl-v31"))
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    output = args.output.resolve()
    source = output / "vndk"
    lib_dir = output / "lib64"
    output.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)

    if not (source / ".git").exists():
        run(
            "git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
            "--branch", BRANCH,
            "https://android.googlesource.com/platform/prebuilts/vndk/v31",
            str(source),
        )
    run("git", "sparse-checkout", "set", "arm64/include", cwd=source)

    for name in DEVICE_LIBS:
        remote = run(
            args.adb, "shell",
            f"for d in /system/lib64 /vendor/lib64 /system_ext/lib64; do "
            f"[ -f \"$d/{name}\" ] && echo \"$d/{name}\" && break; done",
            capture=True,
        )
        if not remote:
            raise SystemExit(f"Required device library not found: {name}")
        run(args.adb, "pull", remote.splitlines()[0], str(lib_dir / name))

    include_root = source / "arm64" / "include"
    include_dirs = [
        include_root / "system/libbase/include",
        include_root / "system/core/libutils/include",
        include_root / "system/core/libcutils/include",
        include_root / "system/core/libsync/include",
        include_root / "system/libhidl/base/include",
        include_root / "system/libhidl/transport/include",
        include_root / "system/libhwbinder/include",
        include_root / "system/libfmq/base",
        include_root / "system/libfmq/include",
        include_root / "system/logging/liblog/include_vndk",
    ]
    include_dirs.extend(sorted(include_root.glob(
        "out/soong/.intermediates/system/libhidl/transport/*/*/"
        "android.hidl.*_genc++_headers/gen"
    )))
    missing = [str(path) for path in include_dirs if not path.is_dir()]
    if missing:
        raise SystemExit("Missing VNDK include directories:\n" + "\n".join(missing))

    cmake = output / "hidl-config.cmake"
    quoted_includes = "\n    ".join(f'"{path}"' for path in include_dirs)
    quoted_libs = "\n    ".join(f'"{lib_dir / name}"' for name in DEVICE_LIBS)
    cmake.write_text(
        "set(LCL_ANDROID_HIDL_INCLUDE_DIRS\n    " + quoted_includes + "\n)\n"
        "set(LCL_ANDROID_HIDL_LIBRARIES\n    " + quoted_libs + "\n)\n",
        encoding="utf-8",
    )
    print(cmake)


if __name__ == "__main__":
    main()
