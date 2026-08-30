#!/usr/bin/env python3

import argparse
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import main as lcl_main


class AndroidBuildTargetsTest(unittest.TestCase):
    def test_canonical_android_output_directories(self) -> None:
        self.assertEqual(
            lcl_main.android_build_dir("x86_64"),
            lcl_main.ROOT_DIR / "out" / "android" / "x86_64",
        )
        self.assertEqual(
            lcl_main.android_build_dir("arm64-v8a"),
            lcl_main.ROOT_DIR / "out" / "android" / "aarch64",
        )

    def test_normal_android_build_includes_core_and_raster_service(self) -> None:
        args = argparse.Namespace(
            jobs=4,
            rebuild=False,
            size=1024,
            software_clients=False,
        )
        with (
            mock.patch.object(
                lcl_main, "find_android_ndk", return_value=Path("/ndk")),
            mock.patch.object(
                lcl_main, "android_skia_cmake_args", return_value=[]),
            mock.patch.object(lcl_main.subprocess, "check_call") as check_call,
        ):
            lcl_main.build_android_phone_artifacts(
                args, use_rootfs=False, abi="x86_64")

        self.assertEqual(check_call.call_count, 2)
        configure_command = check_call.call_args_list[0].args[0]
        self.assertEqual(
            configure_command[configure_command.index("-B") + 1],
            str(lcl_main.ROOT_DIR / "out" / "android" / "x86_64"),
        )
        build_command = check_call.call_args_list[1].args[0]
        target_start = build_command.index("--target") + 1
        target_end = build_command.index("-j")
        self.assertEqual(
            build_command[target_start:target_end],
            ["lcl-core-android", "lcl-rasterd-android"],
        )


if __name__ == "__main__":
    unittest.main()
