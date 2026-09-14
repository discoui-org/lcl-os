#!/usr/bin/env python3

import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tooling.build.archive_skia import archive_package
from tooling.build.fetch_skia import (
    _expected_checksum,
    _extract_archive,
    _private_release_asset_urls,
    package_is_valid,
)
from tooling.build.skia_release import SKIA_REVISION, archive_name, release_tag
from tooling.build.skia_package import _require_package


class SkiaPackageTest(unittest.TestCase):
    def make_package(self, root: Path, target: str) -> Path:
        package = root / target
        (package / "include" / "core").mkdir(parents=True)
        (package / "lib").mkdir()
        (package / "include" / "core" / "SkCanvas.h").write_text(
            "// test\n", encoding="utf-8")
        (package / "lib" / "libskia.a").write_bytes(b"skia")
        (package / "lib" / "libfreetype2.a").write_bytes(b"freetype")
        (package / "manifest.json").write_text(
            json.dumps({"skia_revision": SKIA_REVISION, "target": target}),
            encoding="utf-8",
        )
        return package

    def test_archive_is_deterministic_and_extracts_as_one_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            packages = root / "packages"
            target = "host-x86_64"
            self.make_package(packages, target)
            first, _ = archive_package(target, root / "first", packages)
            second, checksum = archive_package(target, root / "second", packages)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            digest = hashlib.sha256(second.read_bytes()).hexdigest()
            self.assertEqual(
                _expected_checksum(checksum.read_bytes(), archive_name(target)),
                digest,
            )
            extracted = _extract_archive(second, root / "extracted", target)
            self.assertTrue(package_is_valid(extracted, target))

    def test_extractor_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "unsafe.tar.gz"
            with tarfile.open(archive, "w:gz") as tar:
                payload = b"escape"
                info = tarfile.TarInfo("host-x86_64/../../escape")
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
            with self.assertRaisesRegex(RuntimeError, "unsafe entry"):
                _extract_archive(archive, root / "output", "host-x86_64")

    def test_release_tag_binds_skia_revision_and_recipe(self) -> None:
        tag = release_tag()
        self.assertTrue(tag.startswith(f"skia-{SKIA_REVISION[:12]}-"))
        self.assertEqual(len(tag.rsplit("-", 1)[1]), 12)

    def test_private_release_resolves_assets_through_github_api(self) -> None:
        metadata = json.dumps({
            "assets": [
                {"name": "SHA256SUMS", "url": "https://api/assets/1"},
                {"name": "lcl-skia-host-x86_64.tar.gz",
                 "url": "https://api/assets/2"},
            ],
        }).encode("utf-8")
        with mock.patch(
                "tooling.build.fetch_skia._download", return_value=metadata) as download:
            urls = _private_release_asset_urls(
                "discoui-org/lcl-os", "skia-test",
                ("SHA256SUMS", "lcl-skia-host-x86_64.tar.gz"), "token")
        self.assertEqual(urls["SHA256SUMS"], "https://api/assets/1")
        self.assertEqual(
            urls["lcl-skia-host-x86_64.tar.gz"], "https://api/assets/2")
        download.assert_called_once()

    def test_build_resolver_fetches_a_missing_package(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "lcl-os"
            packages = project / "out" / "skia" / "packages"

            def install(target: str, **_: object) -> Path:
                return self.make_package(packages, target)

            with mock.patch(
                    "tooling.build.fetch_skia.fetch_package",
                    side_effect=install) as fetch:
                resolved = _require_package(project, "host-x86_64")
            self.assertEqual(resolved, (packages / "host-x86_64").resolve())
            fetch.assert_called_once()


if __name__ == "__main__":
    unittest.main()
