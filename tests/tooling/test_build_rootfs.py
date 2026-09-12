#!/usr/bin/env python3

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tooling.build import build_rootfs


class BuildRootfsTest(unittest.TestCase):
    def test_permission_repair_can_target_only_the_rootfs_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "lcl-rootfs-x86_64.ext4"
            image.touch()

            with (
                mock.patch.dict(os.environ, {"HOST_UID": "1000", "HOST_GID": "1001"}),
                mock.patch.object(build_rootfs.subprocess, "run") as run,
            ):
                build_rootfs.fix_permissions(image)

            run.assert_called_once_with(
                ["chown", "-R", "1000:1001", str(image)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    def test_cached_image_repairs_host_permissions_before_return(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            rootfs_dir = output / "rootfs"
            cache_dir = output / "cache"
            rootfs_dir.mkdir()
            cache_dir.mkdir()
            image = rootfs_dir / "lcl-rootfs-x86_64.ext4"
            fingerprint = cache_dir / "rootfs-x86_64.sha256"
            image.touch()
            fingerprint.write_text("current\n", encoding="utf-8")

            with (
                mock.patch.object(build_rootfs, "ROOTFS_BASE_DIR", rootfs_dir),
                mock.patch.object(build_rootfs, "QEMU_CACHE_DIR", cache_dir),
                mock.patch.object(
                    build_rootfs, "canonical_build_dir", return_value=output / "build"
                ),
                mock.patch.object(build_rootfs, "ensure_binaries", return_value={}),
                mock.patch.object(
                    build_rootfs, "rootfs_input_fingerprint", return_value="current"
                ),
                mock.patch.object(build_rootfs, "fix_permissions") as fix_permissions,
                mock.patch.dict(
                    os.environ, {"LCL_QEMU_BUILDER_IMAGE_ID": "sha256:test"}
                ),
            ):
                result = build_rootfs.build_rootfs_ext4(inside_docker=True)

            self.assertEqual(result, (rootfs_dir / "x86_64", image, {}))
            fix_permissions.assert_called_once_with(image)


if __name__ == "__main__":
    unittest.main()
