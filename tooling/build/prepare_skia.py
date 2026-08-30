#!/usr/bin/env python3
"""Build the small target package consumed by LCL; never vendor Skia sources."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from urllib.request import urlopen


PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.paths import SKIA_PACKAGE_DIR, SKIA_SOURCE_DIR

DEFAULT_SOURCE = SKIA_SOURCE_DIR
PACKAGE_ROOT = SKIA_PACKAGE_DIR
SKIA_REPOSITORY = "https://skia.googlesource.com/skia.git"
SKIA_REVISION = "2a47279addba493bbbc9bd60346af1c87e11bc69"
GN_REVISION = "b2afae122eeb6ce09c52d63f67dc53fc517dbdc8"

COMMON_ARGS = (
    "is_official_build=true",
    "is_debug=false",
    "skia_enable_tools=false",
    "skia_enable_pdf=false",
    "skia_enable_skottie=false",
    "skia_enable_svg=false",
    "skia_enable_ganesh=true",
    "skia_enable_graphite=false",
    "skia_use_gl=true",
    "skia_use_egl=true",
    'skia_gl_standard="gles"',
    "skia_use_vulkan=false",
    "skia_use_fontconfig=false",
    "skia_use_freetype=true",
    "skia_use_system_freetype2=false",
    "skia_use_freetype_svg=false",
    "skia_use_freetype_zlib=false",
    "skia_enable_fontmgr_custom_empty=true",
    "skia_enable_fontmgr_custom_directory=false",
    "skia_use_harfbuzz=false",
    "skia_use_icu=false",
    "skia_use_expat=false",
    "skia_use_perfetto=false",
    "skia_use_partition_alloc=false",
    "skia_use_libjpeg_turbo_decode=false",
    "skia_use_libjpeg_turbo_encode=false",
    "skia_use_libpng_decode=false",
    "skia_use_libpng_encode=false",
    "skia_use_system_libpng=false",
    "skia_use_libwebp_decode=false",
    "skia_use_libwebp_encode=false",
    "skia_use_wuffs=false",
    "skia_use_piex=false",
    "skia_use_zlib=false",
    "skia_use_system_zlib=false",
)


def run(command: list[str], cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def git_command(source: Path, *arguments: str) -> list[str]:
    """Build a path-scoped Git command safe for a bind-mounted checkout."""
    return [
        "git", "-c", f"safe.directory={source}",
        "-C", str(source), *arguments,
    ]


def git_revision(source: Path) -> str | None:
    """Return HEAD, or None when a clone was interrupted before its first commit."""
    try:
        return subprocess.check_output(
            git_command(source, "rev-parse", "--verify", "HEAD"),
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except subprocess.CalledProcessError:
        return None


def find_ndk() -> Path:
    candidates: list[Path] = []
    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if value := os.environ.get(variable):
            candidates.append(Path(value).expanduser())
    ndk_root = Path.home() / "Android" / "Sdk" / "ndk"
    if ndk_root.is_dir():
        candidates.extend(sorted(ndk_root.iterdir(), reverse=True))
    for candidate in candidates:
        if (candidate / "build" / "cmake" / "android.toolchain.cmake").is_file():
            return candidate.resolve()
    raise RuntimeError("Android NDK not found; set ANDROID_NDK_HOME.")


def prepare_source(source: Path) -> None:
    if not (source / ".git").is_dir():
        if not shutil.which("git"):
            raise RuntimeError(
                "Skia source is missing and git is unavailable in this builder."
            )
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", SKIA_REPOSITORY, str(source)])
        run(git_command(source, "checkout", "--detach", SKIA_REVISION))
    if shutil.which("git"):
        revision = git_revision(source)
        if revision is None:
            print(f"[LCL SKIA] Repairing incomplete checkout at {source}...")
            run(git_command(
                source, "fetch", "--depth=1", "origin", SKIA_REVISION,
            ))
            run(git_command(source, "checkout", "--detach", SKIA_REVISION))
            revision = git_revision(source)
    else:
        head = (source / ".git" / "HEAD").read_text(encoding="utf-8").strip()
        if head.startswith("ref: "):
            ref_name = head.removeprefix("ref: ")
            ref_path = source / ".git" / ref_name
            if ref_path.is_file():
                revision = ref_path.read_text(encoding="utf-8").strip()
            else:
                revision = ""
                packed_refs = source / ".git" / "packed-refs"
                if packed_refs.is_file():
                    suffix = f" {ref_name}"
                    for line in packed_refs.read_text(encoding="utf-8").splitlines():
                        if line.endswith(suffix):
                            revision = line.split(" ", 1)[0]
                            break
        else:
            revision = head
    if revision != SKIA_REVISION:
        raise RuntimeError(
            f"{source} is at {revision}; expected pinned Skia {SKIA_REVISION}."
        )
    if not (source / "third_party" / "externals" / "freetype" / ".git").is_dir():
        if not shutil.which("git"):
            raise RuntimeError(
                "Skia dependencies are missing and git is unavailable in this builder."
            )
        run([sys.executable, "tools/git-sync-deps"], cwd=source)


def native_gn(source: Path, machine: str) -> Path:
    """Keep GN host tools per architecture; the Skia checkout is shared."""
    cpu = "arm64" if machine in {"aarch64", "arm64"} else "amd64"
    gn = source / "out" / "lcl-tools" / cpu / "gn"
    if gn.is_file() and os.access(gn, os.X_OK):
        return gn
    gn.parent.mkdir(parents=True, exist_ok=True)
    url = (
        "https://chrome-infra-packages.appspot.com/dl/gn/gn/"
        f"linux-{cpu}/+/git_revision:{GN_REVISION}"
    )
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / "gn.zip"
        archive.write_bytes(urlopen(url).read())
        with zipfile.ZipFile(archive) as zipped:
            zipped.extract("gn", gn.parent)
    gn.chmod(
        stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR |
        stat.S_IRGRP | stat.S_IXGRP |
        stat.S_IROTH | stat.S_IXOTH
    )
    return gn


def build(target: str, source: Path) -> None:
    args = list(COMMON_ARGS)
    libraries = ["skia", "freetype2", "png", "zlib", "skcms"]
    machine = platform.machine().lower()
    if target in {"host-x86_64", "host-aarch64"}:
        expected_machines = (
            {"x86_64", "amd64"} if target == "host-x86_64"
            else {"aarch64", "arm64"}
        )
        if platform.system() != "Linux" or machine not in expected_machines:
            raise RuntimeError(
                f"{target} must be built inside matching Linux "
                f"({', '.join(sorted(expected_machines))})."
            )
        target_cpu = "x64" if target == "host-x86_64" else "arm64"
        args.extend((f'target_cpu="{target_cpu}"', "skia_use_x11=false"))
        output_name = "lcl-host" if target == "host-x86_64" else "lcl-host-arm64"
    elif target in {"android-x86_64", "android-arm64-v8a"}:
        ndk = find_ndk()
        target_cpu = "x64" if target == "android-x86_64" else "arm64"
        args.extend((
            'target_os="android"',
            f'target_cpu="{target_cpu}"',
            f'ndk="{ndk}"',
            "ndk_api=35",
        ))
        libraries.append("cpu-features")
        output_name = (
            "lcl-android-x64" if target == "android-x86_64"
            else "lcl-android-arm64"
        )
    else:
        raise RuntimeError(f"Unsupported Skia package target: {target}")

    gn = native_gn(source, machine)
    ninja = Path(shutil.which("ninja") or
                 source / "third_party" / "ninja" / "ninja")
    output = source / "out" / output_name
    run([str(gn), "gen", str(output), f"--args={' '.join(args)}"], cwd=source)
    run([str(ninja), "-C", str(output), "skia"], cwd=source)

    package = PACKAGE_ROOT / target
    (package / "lib").mkdir(parents=True, exist_ok=True)
    (package / "modules").mkdir(parents=True, exist_ok=True)
    shutil.copytree(source / "include", package / "include", dirs_exist_ok=True)
    shutil.copytree(
        source / "modules" / "skcms", package / "modules" / "skcms",
        dirs_exist_ok=True,
    )
    for library in libraries:
        shutil.copy2(output / f"lib{library}.a", package / "lib")
    (package / "manifest.json").write_text(
        json.dumps({"skia_revision": SKIA_REVISION, "target": target}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    size_mb = sum(path.stat().st_size for path in package.rglob("*") if path.is_file()) / (1024 * 1024)
    print(f"[LCL SKIA] Prepared {package} ({size_mb:.1f} MiB)")

    uid = os.environ.get("HOST_UID")
    gid = os.environ.get("HOST_GID")
    if uid and gid:
        run(["chown", "-R", f"{uid}:{gid}", str(output), str(package)])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--target", required=True,
        choices=(
            "host-x86_64", "host-aarch64",
            "android-x86_64", "android-arm64-v8a",
        ),
    )
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    options = parser.parse_args()
    source = options.source.expanduser().resolve()
    prepare_source(source)
    build(options.target, source)


if __name__ == "__main__":
    main()
