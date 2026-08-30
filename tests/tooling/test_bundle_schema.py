#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tooling.build.build_rootfs import validate_app_bundles


class StrictBundleSchemaTest(unittest.TestCase):
    def test_accepts_manifest_and_resources_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory)
            bundle = staging / "System" / "Applications" / "Valid.app"
            (bundle / "Resources").mkdir(parents=True)
            (bundle / "Executables").mkdir()
            (bundle / "Resources" / "Icon.png").write_bytes(b"icon")
            executable = bundle / "Executables" / "Valid"
            executable.write_bytes(b"binary")
            executable.chmod(0o755)
            (bundle / "Manifest.json").write_text(
                json.dumps({
                    "id": "org.lcl.valid",
                    "name": "Valid",
                    "executable": "Executables/Valid",
                    "icon": "Resources/Icon.png",
                }),
                encoding="utf-8",
            )

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


if __name__ == "__main__":
    unittest.main()
