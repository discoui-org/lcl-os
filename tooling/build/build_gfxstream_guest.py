#!/usr/bin/env python3
"""Build the pinned, headless gfxstream guest Vulkan ICD for an LCL rootfs.

This intentionally produces only the upstream guest ICD.  It does not install
an ICD manifest, select it for any application, or grant a GPU capability.
Those steps belong to the LCL socket transport and native decoder milestones.
"""

from __future__ import annotations

import argparse
import platform
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
GFXSTREAM_ROOT = PROJECT_ROOT / "third_party" / "gfxstream"
EXPECTED_COMMIT = "6247388dea0ce60c1dcb9cfc3572c05e0e912697"


def normalize_arch(value: str | None) -> str:
    candidate = (value or platform.machine()).lower().strip()
    return "aarch64" if candidate in {"aarch64", "arm64", "armv8", "armv9"} else "x86_64"


def pinned_commit() -> str:
    if not (GFXSTREAM_ROOT / ".git").exists():
        raise RuntimeError("third_party/gfxstream is not an initialized git submodule")
    return subprocess.check_output(
        ["git", "-C", str(GFXSTREAM_ROOT), "rev-parse", "HEAD"], text=True
    ).strip()


def build(arch: str) -> Path:
    actual = pinned_commit()
    if actual != EXPECTED_COMMIT:
        raise RuntimeError(
            "gfxstream source is not the approved pin: "
            f"expected {EXPECTED_COMMIT}, got {actual}"
        )

    build_dir = PROJECT_ROOT / "out" / "gfxstream" / normalize_arch(arch)
    artifact = build_dir / "guest" / "vulkan" / "libvulkan_gfxstream.so"
    options = [
        "-Dgfxstream-build=guest",
        "-Dplatforms=[]",
        "-Degl-native-platform=surfaceless",
        "-Dgallium-drivers=[]",
        "-Dvulkan-drivers=gfxstream-experimental",
        "-Dbuild-tests=false",
        "-Dbuildtype=release",
    ]
    if (build_dir / "meson-private" / "coredata.dat").is_file():
        command = ["meson", "setup", "--reconfigure", str(build_dir), str(GFXSTREAM_ROOT), *options]
    else:
        command = ["meson", "setup", str(build_dir), str(GFXSTREAM_ROOT), *options]
    subprocess.run(command, check=True)
    subprocess.run(
        ["ninja", "-C", str(build_dir), "guest/vulkan/libvulkan_gfxstream.so"],
        check=True,
    )
    if not artifact.is_file():
        raise RuntimeError(f"gfxstream guest build did not produce {artifact}")
    return artifact


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default=None, help="Target architecture (x86_64 or aarch64)")
    args = parser.parse_args()
    artifact = build(normalize_arch(args.arch))
    print(artifact)


if __name__ == "__main__":
    main()
