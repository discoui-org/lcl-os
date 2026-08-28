#!/usr/bin/env python3
"""
LCL OS - Android AVD Developer Launcher (Pure Python - Stage 4C.3)
Builds canonical desktop applications, produces canonical ext4 rootfs,
builds Android composition root (lcl-core-android), generates bootable Android images,
attaches the exact canonical ext4 rootfs as a secondary virtio block disk,
and boots Android AVD with visible GUI window.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from skia_package import android_skia_cmake_args

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
BUILD_DIR = ROOT_DIR / "build"
BUILD_ANDROID_DIR = ROOT_DIR / "build-android"
CANONICAL_ROOTFS_EXT4 = BUILD_DIR / "rootfs" / "lcl-rootfs-x86_64.ext4"


def log(msg: str) -> None:
    print(f"[LCL AVD] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL AVD ERROR] {msg}", file=sys.stderr, flush=True)


def get_sha256(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


class AndroidEnvironment:
    def __init__(self, sdk_root: Path | None = None, ndk_root: Path | None = None):
        self.sdk_root = sdk_root or self._find_sdk_root()
        self.adb = self._find_tool("adb", ["platform-tools/adb"])
        self.emulator = self._find_tool("emulator", ["emulator/emulator"])
        self.ndk_root = ndk_root or self._find_ndk_root()

    def _find_sdk_root(self) -> Path:
        candidates = [
            os.environ.get("ANDROID_HOME"),
            os.environ.get("ANDROID_SDK_ROOT"),
            str(Path.home() / "Android/Sdk"),
            str(Path.home() / "Android/sdk"),
            "/opt/android-sdk",
            "/usr/lib/android-sdk",
        ]
        for c in candidates:
            if c and Path(c).is_dir():
                return Path(c)
        raise RuntimeError("Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT.")

    def _find_tool(self, tool_name: str, rel_paths: list[str]) -> Path:
        if self.sdk_root:
            for rp in rel_paths:
                p = self.sdk_root / rp
                if p.is_file() and os.access(p, os.X_OK):
                    return p
        w = shutil.which(tool_name)
        if w:
            return Path(w)
        raise RuntimeError(f"Required Android tool '{tool_name}' not found.")

    def _find_ndk_root(self) -> Path | None:
        ndk_env = os.environ.get("ANDROID_NDK_ROOT") or os.environ.get("NDK_HOME")
        if ndk_env and Path(ndk_env).is_dir():
            return Path(ndk_env)
        ndk_dir = self.sdk_root / "ndk"
        if ndk_dir.is_dir():
            versions = sorted([d for d in ndk_dir.iterdir() if d.is_dir()], reverse=True)
            if versions:
                return versions[0]
        return None

    def run_adb(self, *args: str, capture_output: bool = False, check: bool = True, quiet: bool = True) -> subprocess.CompletedProcess:
        cmd = [str(self.adb)] + list(args)
        stdout = subprocess.PIPE if capture_output else (subprocess.DEVNULL if quiet else None)
        stderr = subprocess.PIPE if capture_output else (subprocess.DEVNULL if quiet else None)
        return subprocess.run(cmd, stdout=stdout, stderr=stderr, text=True, check=check)

    def adb_shell(self, cmd_str: str, capture_output: bool = True, check: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run([str(self.adb), "shell", cmd_str], capture_output=capture_output, text=True, check=check)


def is_android_core_stale(android_core: Path) -> bool:
    if not android_core.is_file():
        return True
    core_mtime = android_core.stat().st_mtime
    for d in (
        ROOT_DIR / "src" / "platform" / "android",
        ROOT_DIR / "src" / "platform" / "common",
        ROOT_DIR / "src" / "render",
        ROOT_DIR / "lcl-graphics",
    ):
        if d.is_dir():
            for p in d.rglob("*"):
                if p.is_file() and p.stat().st_mtime > core_mtime:
                    return True
    for build_input in (
        ROOT_DIR / "CMakeLists.txt",
        ROOT_DIR / "cmake" / "LclSkia.cmake",
    ):
        if build_input.is_file() and build_input.stat().st_mtime > core_mtime:
            return True
    return False


def is_android_images_stale(out_system_img: Path, out_ramdisk_img: Path,
                            android_core: Path, rasterd: Path) -> bool:
    if not out_system_img.is_file() or not out_ramdisk_img.is_file():
        return True
    sys_mtime = out_system_img.stat().st_mtime
    if android_core.is_file() and android_core.stat().st_mtime > sys_mtime:
        return True
    if rasterd.is_file() and rasterd.stat().st_mtime > sys_mtime:
        return True
    scripts = [
        SCRIPT_DIR / "build_android_images.py",
        ROOT_DIR / "src" / "platform" / "android" / "lcl.rc",
        ROOT_DIR / "src" / "platform" / "android" / "lcl-bootstrap.sh",
    ]
    for s in scripts:
        if s.is_file() and s.stat().st_mtime > sys_mtime:
            return True
    return False


def build_targets(env: AndroidEnvironment, force_rebuild: bool = False, arch: str = "x86_64") -> None:
    # 1. Update userspace and package the canonical rootfs in one Docker run.
    # CMake caches created under the container's /src mount are intentionally
    # never reopened from the host checkout path.
    log("Ensuring canonical Linux userspace and rootfs are up to date (Docker)...")
    sys.path.insert(0, str(SCRIPT_DIR))
    from build_rootfs import build_rootfs_ext4
    build_rootfs_ext4(arch=arch, force=force_rebuild)

    rootfs_sha = get_sha256(CANONICAL_ROOTFS_EXT4)
    log(f"Canonical ext4 rootfs ready ({CANONICAL_ROOTFS_EXT4.stat().st_size} bytes, SHA-256: {rootfs_sha})")

    # 2. Build Android Platform Composition Root (lcl-core-android)
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"
    rasterd = BUILD_ANDROID_DIR / "lcl-rasterd-android"
    if force_rebuild or is_android_core_stale(android_core) or not rasterd.is_file():
        log("Building Android compositor and raster service...")
        if not env.ndk_root:
            raise RuntimeError("Android NDK not found. Set ANDROID_NDK_ROOT.")
        toolchain = env.ndk_root / "build/cmake/android.toolchain.cmake"
        subprocess.run([
            "cmake", "-B", str(BUILD_ANDROID_DIR), "-S", str(ROOT_DIR),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            "-DANDROID_ABI=x86_64",
            "-DANDROID_PLATFORM=android-35",
            "-DBUILD_TESTS=OFF",
            *android_skia_cmake_args(ROOT_DIR, "x86_64"),
        ], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([
            "cmake", "--build", str(BUILD_ANDROID_DIR),
            "--target", "lcl-core-android", "lcl-rasterd-android",
            "-j", str(os.cpu_count() or 4)
        ], check=True, stdout=subprocess.DEVNULL)


def is_device_online(env: AndroidEnvironment) -> bool:
    try:
        res = env.run_adb("devices", capture_output=True, check=False)
        for line in res.stdout.splitlines():
            parts = line.strip().split()
            if len(parts) >= 2 and parts[1] == "device":
                return True
    except Exception:
        pass
    return False


def clean_stale_avd_locks(avd_name: str) -> None:
    # Kill any dangling emulator or qemu processes from prior interrupted runs
    subprocess.run(["killall", "-9", "qemu-system-x86_64", "emulator"], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for base in [Path.home() / ".android/avd", Path.home() / ".config/.android/avd"]:
        avd_dir = base / f"{avd_name}.avd"
        if avd_dir.is_dir():
            for lock_item in avd_dir.rglob("*.lock"):
                try:
                    if lock_item.is_dir():
                        shutil.rmtree(lock_item)
                    else:
                        lock_item.unlink()
                except Exception:
                    pass


def launch_avd(args: argparse.Namespace) -> None:
    # Ensure Android emulator GUI uses XCB/X11 compatibility on Wayland sessions
    if "QT_QPA_PLATFORM" not in os.environ and os.environ.get("DISPLAY"):
        os.environ["QT_QPA_PLATFORM"] = "xcb"

    env = AndroidEnvironment()

    avd_name = args.avd_name or "lcl-phone"

    # Verify AVD exists
    res = subprocess.run([str(env.emulator), "-list-avds"], capture_output=True, text=True, check=True)
    available_avds = [a.strip() for a in res.stdout.splitlines() if a.strip()]
    if avd_name not in available_avds:
        err(f"AVD '{avd_name}' not found. Available AVDs: {available_avds}")
        sys.exit(1)

    # 1. Build targets & generate custom bootable LCL images
    out_system_img = BUILD_DIR / "android" / "lcl-system.img"
    out_ramdisk_img = BUILD_DIR / "android" / "lcl-ramdisk.img"
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"
    rasterd = BUILD_ANDROID_DIR / "lcl-rasterd-android"

    if not args.no_build:
        build_targets(env, force_rebuild=args.rebuild, arch=getattr(args, "arch", "x86_64") or "x86_64")
        if args.rebuild or is_android_images_stale(
                out_system_img, out_ramdisk_img, android_core, rasterd):
            sys.path.insert(0, str(SCRIPT_DIR))
            from build_android_images import build_android_images
            out_system_img, out_ramdisk_img = build_android_images()
        else:
            log("Android substrate images (system & ramdisk) are up to date.")
    elif not out_system_img.is_file() or not out_ramdisk_img.is_file() or not CANONICAL_ROOTFS_EXT4.is_file():
        err("Missing required AVD images or canonical rootfs for --no-build.")
        sys.exit(1)

    rootfs_sha = get_sha256(CANONICAL_ROOTFS_EXT4)
    log(f"Attaching exact canonical rootfs artifact: {CANONICAL_ROOTFS_EXT4.name} (SHA-256: {rootfs_sha})")

    # 2. Start emulator directly with custom LCL system image, ramdisk, and canonical ext4 rootfs disk attachment
    clean_stale_avd_locks(avd_name)
    log(f"Booting {avd_name} directly with custom LCL substrate & canonical ext4 rootfs...")

    emu_cmd = [
        str(env.emulator),
        "-avd", avd_name,
        "-no-snapshot-load",
        "-no-boot-anim",
        "-selinux", "permissive",
    ]
    if args.show_kernel:
        emu_cmd.append("-show-kernel")
    if args.no_window:
        emu_cmd.append("-no-window")

    # QEMU arguments: Attach canonical rootfs as secondary raw block device via -blockdev
    emu_cmd.extend([
        "-qemu",
        "-cpu", "host",
        "-blockdev", f"driver=file,filename={CANONICAL_ROOTFS_EXT4},node-name=lcl_rootfs_file",
        "-blockdev", "driver=raw,file=lcl_rootfs_file,node-name=lcl_rootfs",
        "-device", "virtio-blk-pci,drive=lcl_rootfs",
    ])

    emulator_proc = subprocess.Popen(
        emu_cmd,
        stdout=None if args.show_kernel else subprocess.DEVNULL,
        stderr=None if args.show_kernel else subprocess.DEVNULL
    )

    try:
        # Diagnostic monitoring
        log("Waiting for LCL OS startup on AVD...")
        time.sleep(2)
        env.run_adb("wait-for-device", check=False, quiet=True)

        # Poll for LCL Compositor and Session Daemon socket readiness
        log("Waiting for LCL sockets on /Runtime...")
        compositor_ready = False
        session_ready = False
        for _ in range(60):
            if not compositor_ready:
                res_comp = env.adb_shell("[ -S /Runtime/lcl-compositor.sock ] && echo READY")
                if "READY" in res_comp.stdout:
                    compositor_ready = True
            if not session_ready:
                res_sess = env.adb_shell("[ -S /Runtime/lcl-sessiond.sock ] && echo READY")
                if "READY" in res_sess.stdout:
                    session_ready = True
            if compositor_ready and session_ready:
                break
            time.sleep(0.5)

        if compositor_ready and session_ready:
            log("============================================================")
            log("LCL OS is running directly on Android AVD (Stage 4C.3 Canonical RootFS)!")
            log("Active components: Compositor, Session Daemon, Desktop Shell, Terminal")
            log("Press Ctrl+C to stop session.")
            log("============================================================")
        elif compositor_ready:
            log("Compositor socket ready. Session Daemon pending. System is running.")
        else:
            log("Compositor and Session sockets pending. System is running.")

        # Keep-alive monitor loop
        while True:
            time.sleep(1)
            if emulator_proc.poll() is not None:
                log("Emulator process terminated.")
                break

    except KeyboardInterrupt:
        print("")
        log("Received interrupt signal. Shutting down LCL session...")
    finally:
        log("Stopping emulator...")
        if emulator_proc:
            emulator_proc.terminate()
            try:
                emulator_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                emulator_proc.kill()
        env.run_adb("emu", "kill", check=False, quiet=True)
        clean_stale_avd_locks(avd_name)
        log("LCL AVD session closed cleanly.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="LCL OS - Android AVD Developer Launcher"
    )
    parser.add_argument("--arch", "-a", default="x86_64", help="Target architecture (only x86_64 supported on AVD)")
    parser.add_argument("--avd-name", default="lcl-phone", help="Target AVD name (default: lcl-phone)")
    parser.add_argument("--show-kernel", action="store_true", help="Display live guest kernel and init boot logs in terminal")
    parser.add_argument("--no-window", action="store_true", help="Run emulator headless without GUI window")
    parser.add_argument("--no-build", action="store_true", help="Skip build step")
    parser.add_argument("--rebuild", action="store_true", help="Force clean rebuild of all targets")
    args = parser.parse_args()

    if getattr(args, "rebuild", False) and getattr(args, "no_build", False):
        parser.error("Cannot specify both --rebuild and --no-build.")

    arch = args.arch.lower().strip()
    if arch not in ("x86_64", "amd64", "x64"):
        err(f"AVD platform substrate only supports 'x86_64' (requested: '{args.arch}').")
        sys.exit(1)

    launch_avd(args)


if __name__ == "__main__":
    main()
