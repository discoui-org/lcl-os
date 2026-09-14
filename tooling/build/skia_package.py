"""Resolve the target-specific binary package for LCL's shared Skia replay."""

from __future__ import annotations

import os
from pathlib import Path

from tooling.build.skia_release import DEFAULT_GITHUB_REPOSITORY
from tooling.paths import SKIA_PACKAGE_DIR


def _require_package(project_root: Path, package_name: str) -> Path:
    if project_root.resolve() != SKIA_PACKAGE_DIR.parents[2]:
        package = project_root / "out" / "skia" / "packages" / package_name
    else:
        package = SKIA_PACKAGE_DIR / package_name
    from tooling.build.fetch_skia import fetch_package, package_is_valid

    if not package_is_valid(package, package_name):
        repository = os.environ.get(
            "LCL_SKIA_GITHUB_REPOSITORY", DEFAULT_GITHUB_REPOSITORY)
        try:
            fetch_package(
                package_name,
                repository=repository,
                package_root=package.parent,
            )
        except RuntimeError as exc:
            raise RuntimeError(
                f"Skia package '{package_name}' is unavailable at {package}. "
                "Run the GitHub 'Build Skia SDK packages' workflow, then retry "
                "the build. Set GITHUB_TOKEN for a private repository. "
                f"Download error: {exc}"
            ) from exc
        if not package_is_valid(package, package_name):
            raise RuntimeError(
                f"downloaded Skia package '{package_name}' failed validation")
    return package.resolve()


def host_skia_cmake_args(project_root: Path, arch: str) -> list[str]:
    normalized = "x86_64" if arch in {"x64", "amd64"} else arch
    package = _require_package(project_root, f"host-{normalized}")
    return [f"-DLCL_SKIA_ROOT={package}"]


def android_skia_cmake_args(project_root: Path, abi: str) -> list[str]:
    package_arch = {
        "x86_64": "x86_64",
        "arm64-v8a": "arm64-v8a",
    }.get(abi)
    if package_arch is None:
        raise RuntimeError(f"Unsupported Android Skia ABI: {abi}")
    package = _require_package(project_root, f"android-{package_arch}")
    return [f"-DLCL_SKIA_ROOT={package}"]
