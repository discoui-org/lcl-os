#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tooling.build.build_rootfs import validate_app_bundles


class StrictBundleSchemaTest(unittest.TestCase):
    def create_bundle(self, staging: Path, name: str, manifest: dict) -> Path:
        bundle = staging / "System" / "Applications" / f"{name}.app"
        (bundle / "Resources").mkdir(parents=True)
        (bundle / "Executables").mkdir()
        (bundle / "Resources" / "Icon.png").write_bytes(b"icon")
        executable = bundle / "Executables" / name
        executable.write_bytes(b"binary")
        executable.chmod(0o755)
        (bundle / "Manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return bundle

    def test_accepts_manifest_and_resources_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            self.create_bundle(staging, "Valid", {
                "id": "org.lcl.valid",
                "name": "Valid",
                "executable": "Executables/Valid",
                "icon": "Resources/Icon.png",
                "requestedPermissions": ["network.client", "files.user-selected"],
            })

            validate_app_bundles(staging)

    def test_rejects_metadata_only_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            bundle = staging / "System" / "Applications" / "Legacy.app"
            (bundle / "Resources").mkdir(parents=True)
            (bundle / "metadata.json").write_text(
                '{"id":"org.lcl.legacy","name":"Legacy",'
                '"executable":"Executables/Legacy"}',
                encoding="utf-8",
            )

            with self.assertRaisesRegex(RuntimeError, "missing Manifest.json"):
                validate_app_bundles(staging)

    def test_rejects_executable_outside_the_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            self.create_bundle(staging, "Outside", {
                "id": "org.lcl.outside",
                "name": "Outside",
                "executable": "/System/Core/lcl-core",
                "icon": "Resources/Icon.png",
            })

            with self.assertRaisesRegex(RuntimeError, "executable under Executables"):
                validate_app_bundles(staging)

    def test_rejects_symlinked_executable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            bundle = self.create_bundle(staging, "Symlinked", {
                "id": "org.lcl.symlinked",
                "name": "Symlinked",
                "executable": "Executables/Symlinked",
                "icon": "Resources/Icon.png",
            })
            external_executable = staging / "outside-program"
            external_executable.write_bytes(b"binary")
            external_executable.chmod(0o755)
            executable = bundle / "Executables" / "Symlinked"
            executable.unlink()
            executable.symlink_to(external_executable)

            with self.assertRaisesRegex(RuntimeError, "executable not found inside Executables"):
                validate_app_bundles(staging)

    def test_rejects_hard_linked_executable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            bundle = self.create_bundle(staging, "HardLinked", {
                "id": "org.lcl.hard-linked",
                "name": "Hard Linked",
                "executable": "Executables/HardLinked",
                "icon": "Resources/Icon.png",
            })
            external_executable = staging / "outside-program"
            external_executable.write_bytes(b"binary")
            external_executable.chmod(0o755)
            executable = bundle / "Executables" / "HardLinked"
            executable.unlink()
            executable.hardlink_to(external_executable)

            with self.assertRaisesRegex(RuntimeError, "executable not found inside Executables"):
                validate_app_bundles(staging)

    def test_rejects_invalid_permissions_and_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            self.create_bundle(staging, "InvalidPermission", {
                "id": "org.lcl.invalid-permission",
                "name": "Invalid Permission",
                "executable": "Executables/InvalidPermission",
                "icon": "Resources/Icon.png",
                "requestedPermissions": ["Network.Client"],
            })

            with self.assertRaisesRegex(RuntimeError, "invalid requestedPermissions"):
                validate_app_bundles(staging)

        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            bundle = self.create_bundle(staging, "DuplicateKey", {
                "id": "org.lcl.duplicate-key",
                "name": "Duplicate Key",
                "executable": "Executables/DuplicateKey",
                "icon": "Resources/Icon.png",
            })
            (bundle / "Manifest.json").write_text(
                '{"id":"org.lcl.duplicate-key","id":"org.lcl.other",'
                '"name":"Duplicate Key","executable":"Executables/DuplicateKey",'
                '"icon":"Resources/Icon.png"}',
                encoding="utf-8",
            )

            with self.assertRaisesRegex(RuntimeError, "duplicate key 'id'"):
                validate_app_bundles(staging)


if __name__ == "__main__":
    unittest.main()
