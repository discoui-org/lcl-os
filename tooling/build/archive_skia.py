#!/usr/bin/env python3
"""Create a deterministic release archive from one prepared Skia package."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import sys
import tarfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.build.skia_release import (
    PACKAGE_TARGETS,
    SKIA_REVISION,
    archive_name,
)
from tooling.paths import SKIA_PACKAGE_DIR


def validate_package(package: Path, target: str) -> None:
    required = (
        package / "include" / "core" / "SkCanvas.h",
        package / "lib" / "libskia.a",
        package / "lib" / "libfreetype2.a",
        package / "manifest.json",
    )
    if not all(path.is_file() for path in required):
        raise RuntimeError(f"Skia package is incomplete: {package}")
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("target") != target or manifest.get("skia_revision") != SKIA_REVISION:
        raise RuntimeError(f"Skia package manifest does not match {target}")


def archive_package(target: str, output_dir: Path,
                    package_root: Path = SKIA_PACKAGE_DIR) -> tuple[Path, Path]:
    if target not in PACKAGE_TARGETS:
        raise RuntimeError(f"Unsupported Skia package target: {target}")
    package = package_root / target
    validate_package(package, target)
    output_dir.mkdir(parents=True, exist_ok=True)
    archive = output_dir / archive_name(target)

    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as tar:
                paths = [package, *sorted(package.rglob("*"))]
                for path in paths:
                    relative = path.relative_to(package_root).as_posix()
                    info = tar.gettarinfo(str(path), arcname=relative)
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    info.mtime = 0
                    if info.isfile():
                        with path.open("rb") as source:
                            tar.addfile(info, source)
                    else:
                        tar.addfile(info)

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum = archive.with_suffix(archive.suffix + ".sha256")
    checksum.write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
    return archive, checksum


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, choices=PACKAGE_TARGETS)
    parser.add_argument("--output-dir", type=Path, required=True)
    options = parser.parse_args()
    archive, checksum = archive_package(
        options.target, options.output_dir.expanduser().resolve())
    print(f"[LCL SKIA] Archived {archive}")
    print(f"[LCL SKIA] Checksum {checksum}")


if __name__ == "__main__":
    main()
