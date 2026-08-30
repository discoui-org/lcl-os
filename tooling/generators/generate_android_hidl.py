#!/usr/bin/env python3
"""Generate the Android Composer HIDL C++ headers used by LCL OS.

The inputs are pinned to an official AOSP release branch.  Generated output is
checked in so Android cross-builds do not require a full Android source tree.
"""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.paths import CACHE_ROOT


BRANCH = "android12-qpr3-release"
PACKAGES = (
    "android.hardware.graphics.common@1.0",
    "android.hardware.graphics.common@1.1",
    "android.hardware.graphics.common@1.2",
    "android.hardware.graphics.composer@2.1",
    "android.hardware.graphics.composer@2.2",
    "android.hardware.graphics.composer@2.3",
    "android.hardware.graphics.composer@2.4",
)


def run(*args: str, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    subprocess.run(args, cwd=cwd, env=env, check=True)


def sparse_clone(url: str, target: Path, paths: tuple[str, ...]) -> None:
    if not (target / ".git").exists():
        run("git", "clone", "--depth", "1", "--filter=blob:none", "--sparse",
            "--branch", BRANCH, url, str(target))
    run("git", "sparse-checkout", "set", *paths, cwd=target)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cache", type=Path,
        default=CACHE_ROOT / "android-hidl-generator",
    )
    parser.add_argument(
        "--output", type=Path,
        default=PROJECT_ROOT / "platforms/android/generated/hidl/include",
    )
    args = parser.parse_args()

    cache = args.cache.resolve()
    output = args.output.resolve()
    cache.mkdir(parents=True, exist_ok=True)

    interfaces = cache / "hardware-interfaces"
    libhidl = cache / "libhidl"
    sparse_clone(
        "https://android.googlesource.com/platform/hardware/interfaces",
        interfaces,
        ("graphics/common", "graphics/composer"),
    )
    sparse_clone(
        "https://android.googlesource.com/platform/system/libhidl",
        libhidl,
        ("transport/base",),
    )

    tool_root = cache / "build-tools" / "linux-x86"
    tool_root.mkdir(parents=True, exist_ok=True)
    hidl_gen = tool_root / "bin" / "hidl-gen"
    if not hidl_gen.exists():
        url = (
            "https://android.googlesource.com/platform/prebuilts/build-tools/+/refs/heads/"
            f"{BRANCH}/linux-x86/bin/hidl-gen?format=TEXT"
        )
        hidl_gen.parent.mkdir(parents=True, exist_ok=True)
        hidl_gen.write_bytes(base64.b64decode(urllib.request.urlopen(url).read()))
        hidl_gen.chmod(0o755)

    # hidl-gen's host-side shared objects are fetched as an official sparse checkout.
    build_tools = cache / "build-tools-source"
    sparse_clone(
        "https://android.googlesource.com/platform/prebuilts/build-tools",
        build_tools,
        ("linux-x86/lib64",),
    )
    lib64 = build_tools / "linux-x86" / "lib64"

    staging = cache / "generated"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(lib64)
    for package in PACKAGES:
        run(
            str(hidl_gen), "-o", str(staging), "-Lc++-headers",
            "-r", f"android.hardware:{interfaces}",
            "-r", f"android.hidl:{libhidl / 'transport'}",
            package,
            env=env,
        )

    for version in ("2.1", "2.2", "2.3", "2.4"):
        source = (
            interfaces / "graphics" / "composer" / version / "utils" /
            "command-buffer" / "include" / "composer-command-buffer" / version /
            "ComposerCommandBuffer.h"
        )
        destination = staging / "composer-command-buffer" / version / source.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    if output.exists():
        shutil.rmtree(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(staging, output)
    (output.parent / "PROVENANCE.md").write_text(
        "# Android HIDL generated sources\n\n"
        f"Generated from official AOSP branch `{BRANCH}` by "
        "`tooling/generators/generate_android_hidl.py`.\n\n"
        "Inputs:\n"
        "- https://android.googlesource.com/platform/hardware/interfaces\n"
        "- https://android.googlesource.com/platform/system/libhidl\n"
        "- https://android.googlesource.com/platform/prebuilts/build-tools\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
