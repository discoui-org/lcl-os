#!/usr/bin/env python3

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tooling.build import prepare_skia


class PrepareSkiaSourceTest(unittest.TestCase):
    def test_repairs_interrupted_clone_without_deleting_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "skia"
            (source / ".git").mkdir(parents=True)
            (source / "third_party" / "externals" / "freetype" / ".git").mkdir(
                parents=True
            )

            missing_head = subprocess.CalledProcessError(128, ["git"])
            with (
                mock.patch.object(prepare_skia.shutil, "which", return_value="/usr/bin/git"),
                mock.patch.object(
                    prepare_skia.subprocess,
                    "check_output",
                    side_effect=[missing_head, prepare_skia.SKIA_REVISION],
                ),
                mock.patch.object(prepare_skia, "run") as run,
            ):
                prepare_skia.prepare_source(source)

            self.assertEqual(
                run.call_args_list,
                [
                    mock.call(prepare_skia.git_command(
                        source, "fetch", "--depth=1", "origin",
                        prepare_skia.SKIA_REVISION,
                    )),
                    mock.call(prepare_skia.git_command(
                        source, "checkout", "--detach",
                        prepare_skia.SKIA_REVISION,
                    )),
                ],
            )

    def test_rejects_valid_checkout_at_wrong_revision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "skia"
            (source / ".git").mkdir(parents=True)
            (source / "third_party" / "externals" / "freetype" / ".git").mkdir(
                parents=True
            )

            with (
                mock.patch.object(prepare_skia.shutil, "which", return_value="/usr/bin/git"),
                mock.patch.object(
                    prepare_skia.subprocess,
                    "check_output",
                    return_value="wrong-revision",
                ),
                self.assertRaisesRegex(RuntimeError, "expected pinned Skia"),
            ):
                prepare_skia.prepare_source(source)


if __name__ == "__main__":
    unittest.main()
