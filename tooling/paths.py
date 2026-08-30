"""Canonical repository and generated-output paths for LCL tooling."""

from __future__ import annotations

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
OUT_ROOT = PROJECT_ROOT / "out"

HOST_BUILD_DIR = OUT_ROOT / "host"
QEMU_BUILD_ROOT = OUT_ROOT / "qemu"
ANDROID_BUILD_ROOT = OUT_ROOT / "android"
ROOTFS_DIR = OUT_ROOT / "rootfs"

IMAGES_ROOT = OUT_ROOT / "images"
ANDROID_IMAGES_DIR = IMAGES_ROOT / "android"
ISO_IMAGES_DIR = IMAGES_ROOT / "iso"
UTM_OUTPUT_DIR = OUT_ROOT / "utm"

CACHE_ROOT = OUT_ROOT / "cache"
QEMU_CACHE_DIR = CACHE_ROOT / "qemu"
TOOLS_ROOT = OUT_ROOT / "tools"
ANDROID_TOOLS_DIR = TOOLS_ROOT / "android"
ANDROID_HIDL_ROOT = ANDROID_TOOLS_DIR / "hidl-v31"
ANDROID_ZSTD_BINARY = ANDROID_TOOLS_DIR / "arm64" / "zstd"

SKIA_ROOT = OUT_ROOT / "skia"
SKIA_SOURCE_DIR = SKIA_ROOT / "src"
SKIA_PACKAGE_DIR = SKIA_ROOT / "packages"
VISUAL_BASELINES_DIR = OUT_ROOT / "visual-baselines"


def qemu_build_dir(arch: str) -> Path:
    """Return the isolated CMake/QEMU output tree for an architecture."""
    return QEMU_BUILD_ROOT / arch


def android_build_dir(abi: str) -> Path:
    """Return the isolated Android CMake output tree for an ABI."""
    normalized = "aarch64" if abi in {"aarch64", "arm64", "arm64-v8a"} else abi
    return ANDROID_BUILD_ROOT / normalized
