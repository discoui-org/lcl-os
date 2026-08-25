"""Resolve the target-specific binary package for LCL's shared Skia replay."""

from __future__ import annotations

from pathlib import Path


def _require_package(project_root: Path, package_name: str) -> Path:
    package = project_root / "out" / "skia-package" / package_name
    required = (
        package / "include" / "core" / "SkCanvas.h",
        package / "lib" / "libskia.a",
        package / "lib" / "libfreetype2.a",
    )
    if not all(path.is_file() for path in required):
        raise RuntimeError(
            f"Skia package '{package_name}' is not prepared at {package}. "
            f"Run scripts/prepare_skia.py --target {package_name} on the "
            "matching Linux architecture. Rootfs builds prepare it inside "
            "their target Docker automatically."
        )
    return package.resolve()


def host_skia_cmake_args(project_root: Path, arch: str) -> list[str]:
    normalized = "x86_64" if arch in {"x64", "amd64"} else arch
    package = _require_package(project_root, f"host-{normalized}")
    return ["-DLCL_ENABLE_SKIA=ON", f"-DLCL_SKIA_ROOT={package}"]


def android_skia_cmake_args(project_root: Path, abi: str) -> list[str]:
    package_arch = {
        "x86_64": "x86_64",
        "arm64-v8a": "arm64-v8a",
    }.get(abi)
    if package_arch is None:
        raise RuntimeError(f"Unsupported Android Skia ABI: {abi}")
    package = _require_package(project_root, f"android-{package_arch}")
    return ["-DLCL_ENABLE_SKIA=ON", f"-DLCL_SKIA_ROOT={package}"]
