"""Pinned metadata shared by Skia builders, releases, and consumers."""

from __future__ import annotations

import hashlib
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]

SKIA_REPOSITORY = "https://skia.googlesource.com/skia.git"
SKIA_REVISION = "2a47279addba493bbbc9bd60346af1c87e11bc69"
GN_REVISION = "b2afae122eeb6ce09c52d63f67dc53fc517dbdc8"
ANDROID_NDK_REVISION = "28.2.13676358"

PACKAGE_TARGETS = (
    "host-x86_64",
    "host-aarch64",
    "android-x86_64",
    "android-arm64-v8a",
)

DEFAULT_GITHUB_REPOSITORY = "discoui-org/lcl-os"
_RECIPE_PATHS = (
    "tooling/build/prepare_skia.py",
    "tooling/build/skia_release.py",
)


def recipe_digest(project_root: Path = PROJECT_ROOT) -> str:
    digest = hashlib.sha256()
    for relative in _RECIPE_PATHS:
        path = project_root / relative
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def release_tag(project_root: Path = PROJECT_ROOT) -> str:
    return f"skia-{SKIA_REVISION[:12]}-{recipe_digest(project_root)[:12]}"


def archive_name(target: str) -> str:
    if target not in PACKAGE_TARGETS:
        raise ValueError(f"Unsupported Skia package target: {target}")
    return f"lcl-skia-{target}.tar.gz"
