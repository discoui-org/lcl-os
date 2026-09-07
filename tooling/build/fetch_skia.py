#!/usr/bin/env python3
"""Download and verify target-specific Skia packages from GitHub Releases."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.build.skia_release import (
    DEFAULT_GITHUB_REPOSITORY,
    PACKAGE_TARGETS,
    SKIA_REVISION,
    archive_name,
    release_tag,
)
from tooling.paths import SKIA_PACKAGE_DIR

_MAX_ARCHIVE_BYTES = 512 * 1024 * 1024


def package_is_valid(package: Path, target: str) -> bool:
    required = (
        package / "include" / "core" / "SkCanvas.h",
        package / "lib" / "libskia.a",
        package / "lib" / "libfreetype2.a",
        package / "manifest.json",
    )
    if not all(path.is_file() for path in required):
        return False
    try:
        manifest = json.loads(
            (package / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return manifest.get("target") == target and \
        manifest.get("skia_revision") == SKIA_REVISION


def _download(url: str, token: str | None = None, *,
              accept: str = "application/octet-stream",
              max_bytes: int = _MAX_ARCHIVE_BYTES) -> bytes:
    headers = {
        "Accept": accept,
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "lcl-skia-fetcher/1",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = Request(url, headers=headers)
    try:
        with urlopen(request, timeout=120) as response:
            length = response.headers.get("Content-Length")
            if length and int(length) > max_bytes:
                raise RuntimeError("Skia release asset exceeds the size limit")
            contents = response.read(max_bytes + 1)
    except (HTTPError, URLError, TimeoutError) as exc:
        raise RuntimeError(f"could not download Skia release asset: {url}: {exc}") from exc
    if len(contents) > max_bytes:
        raise RuntimeError("Skia release asset exceeds the size limit")
    return contents


def _private_release_asset_urls(repository: str, tag: str,
                                filenames: tuple[str, ...],
                                token: str) -> dict[str, str]:
    metadata_url = f"https://api.github.com/repos/{repository}/releases/tags/{tag}"
    raw = _download(
        metadata_url,
        token,
        accept="application/vnd.github+json",
        max_bytes=4 * 1024 * 1024,
    )
    try:
        release = json.loads(raw.decode("utf-8"))
        assets = release["assets"]
    except (KeyError, TypeError, ValueError, UnicodeDecodeError) as exc:
        raise RuntimeError("GitHub returned invalid Skia release metadata") from exc
    result = {
        asset.get("name"): asset.get("url")
        for asset in assets
        if isinstance(asset, dict)
    }
    if any(not isinstance(result.get(filename), str) for filename in filenames):
        raise RuntimeError("GitHub Skia release is missing a required asset")
    return {filename: result[filename] for filename in filenames}


def _expected_checksum(checksums: bytes, filename: str) -> str:
    try:
        lines = checksums.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise RuntimeError("Skia SHA256SUMS is not UTF-8") from exc
    matches: list[str] = []
    for line in lines:
        fields = line.split()
        if len(fields) == 2 and fields[1].lstrip("*") == filename:
            matches.append(fields[0].lower())
    if len(matches) != 1 or len(matches[0]) != 64 or any(
            character not in "0123456789abcdef" for character in matches[0]):
        raise RuntimeError(f"Skia SHA256SUMS has no unique digest for {filename}")
    return matches[0]


def _extract_archive(archive: Path, destination: Path, target: str) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, mode="r:gz") as tar:
        members = tar.getmembers()
        if not members:
            raise RuntimeError("Skia package archive is empty")
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or not path.parts or path.parts[0] != target or \
                    ".." in path.parts or not (member.isdir() or member.isfile()):
                raise RuntimeError("Skia package archive contains an unsafe entry")
            output = destination.joinpath(*path.parts)
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
                output.chmod(member.mode & 0o777)
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            if source is None:
                raise RuntimeError("Skia package archive contains an unreadable file")
            with source, output.open("wb") as sink:
                shutil.copyfileobj(source, sink)
            output.chmod(member.mode & 0o777)
    return destination / target


def fetch_package(target: str, *, repository: str = DEFAULT_GITHUB_REPOSITORY,
                  package_root: Path = SKIA_PACKAGE_DIR, force: bool = False) -> Path:
    if target not in PACKAGE_TARGETS:
        raise RuntimeError(f"Unsupported Skia package target: {target}")
    package = package_root / target
    if not force and package_is_valid(package, target):
        return package.resolve()

    tag = release_tag()
    base = f"https://github.com/{repository}/releases/download/{tag}"
    filename = archive_name(target)
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        urls = _private_release_asset_urls(
            repository, tag, ("SHA256SUMS", filename), token)
        checksums = _download(urls["SHA256SUMS"], token)
        archive_bytes = _download(urls[filename], token)
    else:
        checksums = _download(f"{base}/SHA256SUMS")
        archive_bytes = _download(f"{base}/{filename}")
    expected = _expected_checksum(checksums, filename)
    actual = hashlib.sha256(archive_bytes).hexdigest()
    if actual != expected:
        raise RuntimeError(
            f"Skia package checksum mismatch for {filename}: {actual} != {expected}")

    package_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".lcl-skia-", dir=package_root) as temporary:
        temporary_path = Path(temporary)
        archive = temporary_path / filename
        archive.write_bytes(archive_bytes)
        extracted = _extract_archive(archive, temporary_path / "extract", target)
        if not package_is_valid(extracted, target):
            raise RuntimeError(f"downloaded Skia package is invalid for {target}")
        backup = temporary_path / "previous"
        if package.exists():
            package.rename(backup)
        try:
            extracted.rename(package)
        except BaseException:
            if backup.exists() and not package.exists():
                backup.rename(package)
            raise
    print(f"[LCL SKIA] Installed {target} from {repository} release {tag}")
    return package.resolve()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", action="append", choices=PACKAGE_TARGETS)
    parser.add_argument("--all", action="store_true")
    parser.add_argument(
        "--repository",
        default=os.environ.get("LCL_SKIA_GITHUB_REPOSITORY",
                               DEFAULT_GITHUB_REPOSITORY),
    )
    parser.add_argument("--force", action="store_true")
    options = parser.parse_args()
    targets = list(PACKAGE_TARGETS) if options.all else options.target
    if not targets:
        parser.error("select --all or at least one --target")
    for target in targets:
        fetch_package(
            target, repository=options.repository, force=options.force)


if __name__ == "__main__":
    main()
