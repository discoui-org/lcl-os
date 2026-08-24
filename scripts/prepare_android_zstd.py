#!/usr/bin/env python3
"""Build a pinned zstd CLI for ARM64 Android rootfs deployment."""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


ZSTD_REPOSITORY = "https://github.com/facebook/zstd.git"
ZSTD_TAG = "v1.5.7"
ZSTD_COMMIT = "f8745da6ff1ad1e7bab384bd1f9d742439278e99"


def run(*args: str, cwd: Path | None = None, capture: bool = False) -> str:
    completed = subprocess.run(
        args,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return completed.stdout.strip() if capture else ""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument(
        "--output", type=Path, default=Path("build/android-tools-arm64/zstd")
    )
    args = parser.parse_args()

    ndk = args.ndk.resolve()
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        raise SystemExit(f"Android NDK toolchain not found: {toolchain}")

    output = args.output.resolve()
    workspace = output.parent
    source = workspace / "zstd-src"
    build = workspace / "zstd-build"
    workspace.mkdir(parents=True, exist_ok=True)

    if not (source / ".git").is_dir():
        run(
            "git", "clone", "--depth", "1", "--branch", ZSTD_TAG,
            ZSTD_REPOSITORY, str(source),
        )

    revision = run("git", "rev-parse", "HEAD", cwd=source, capture=True)
    if revision != ZSTD_COMMIT:
        raise SystemExit(
            f"Unexpected zstd revision in {source}: {revision} (expected {ZSTD_COMMIT})"
        )

    run(
        "cmake", "-S", str(source / "build/cmake"), "-B", str(build),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-23",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DZSTD_BUILD_PROGRAMS=ON",
        "-DZSTD_BUILD_CONTRIB=OFF",
        "-DZSTD_BUILD_TESTS=OFF",
        "-DZSTD_BUILD_SHARED=OFF",
        "-DZSTD_BUILD_STATIC=ON",
        "-DZSTD_LEGACY_SUPPORT=OFF",
        "-DZSTD_MULTITHREAD_SUPPORT=OFF",
        "-DZSTD_PROGRAMS_LINK_SHARED=OFF",
    )
    run("cmake", "--build", str(build), "--target", "zstd", "-j", "2")

    candidates = (build / "programs/zstd", build / "zstd")
    built_binary = next((path for path in candidates if path.is_file()), None)
    if built_binary is None:
        raise SystemExit(f"ARM64 Android zstd output not found under {build}")

    shutil.copy2(built_binary, output)
    strip_candidates = sorted(
        (ndk / "toolchains/llvm/prebuilt").glob("*/bin/llvm-strip")
    )
    if not strip_candidates:
        raise SystemExit(f"Android NDK llvm-strip not found under {ndk}")
    run(str(strip_candidates[0]), "--strip-unneeded", str(output))
    output.chmod(0o755)
    print(output)


if __name__ == "__main__":
    main()
