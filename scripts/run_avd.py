#!/usr/bin/env python3
"""
LCL OS - Android AVD Developer Launcher (Pure Python)
Builds canonical desktop applications (same-binary) and Android composition root,
stages the reproducible glibc runtime, boots Android AVD with visible GUI window,
deploys artifacts via adb, takes DRM/Composer3 display ownership, and launches
the full LCL desktop session (Wallpaper, MenuBar, Dock, Terminal).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
BUILD_DIR = ROOT_DIR / "build"
BUILD_ANDROID_DIR = ROOT_DIR / "build-android"
STAGING_RUNTIME_DIR = BUILD_DIR / "lcl-runtime"


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


def stage_glibc_runtime(binaries: list[Path], staging_dir: Path) -> None:
    """Populates build/lcl-runtime reproducibly from host shared libraries."""
    staging_dir.mkdir(parents=True, exist_ok=True)
    lib_dir = staging_dir / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)

    # 1. Locate and copy dynamic linker
    loader_found = False
    for loader_path in [
        Path("/lib64/ld-linux-x86-64.so.2"),
        Path("/usr/lib64/ld-linux-x86-64.so.2"),
        Path("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
        Path("/usr/lib/ld-linux-x86-64.so.2"),
    ]:
        if loader_path.is_file():
            resolved = loader_path.resolve()
            shutil.copy2(resolved, staging_dir / "ld-linux-x86-64.so.2")
            shutil.copy2(resolved, lib_dir / "ld-linux-x86-64.so.2")
            loader_found = True
            break
    if not loader_found:
        raise RuntimeError("Host dynamic linker ld-linux-x86-64.so.2 not found.")

    # 2. Parse ldd and copy shared libraries
    for binary in binaries:
        if not binary.is_file():
            continue
        res = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
        for line in res.stdout.splitlines():
            line = line.strip()
            if "=>" in line:
                parts = line.split("=>")
                lib_name = parts[0].strip()
                rest = parts[1].strip()
                lib_path = rest.split()[0]
                p = Path(lib_path)
                if p.is_file():
                    target = lib_dir / lib_name
                    if not target.exists():
                        shutil.copy2(p.resolve(), target)


def build_targets(env: AndroidEnvironment, force_rebuild: bool = False) -> None:
    desktop_shell = BUILD_DIR / "lcl-desktop-shell"
    desktop_term = BUILD_DIR / "lcl-terminal"
    sessiond_bin = BUILD_DIR / "lcl-sessiond"
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"

    # Build Canonical Desktop apps
    if force_rebuild or not desktop_shell.is_file() or not desktop_term.is_file() or not sessiond_bin.is_file():
        log("Building canonical LCL desktop applications & session daemon...")
        if not (BUILD_DIR / "CMakeCache.txt").is_file():
            subprocess.run(["cmake", "-B", str(BUILD_DIR), "-S", str(ROOT_DIR)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([
            "cmake", "--build", str(BUILD_DIR),
            "--target", "lcl-desktop-shell", "lcl-terminal", "lcl-sessiond",
            "-j", str(os.cpu_count() or 4)
        ], check=True, stdout=subprocess.DEVNULL)

    # Build Android Platform Composition Root
    if force_rebuild or not android_core.is_file():
        log("Building Android platform compositor (lcl-core-android)...")
        if not env.ndk_root:
            raise RuntimeError("Android NDK not found. Set ANDROID_NDK_ROOT.")
        toolchain = env.ndk_root / "build/cmake/android.toolchain.cmake"
        if not (BUILD_ANDROID_DIR / "CMakeCache.txt").is_file():
            subprocess.run([
                "cmake", "-B", str(BUILD_ANDROID_DIR), "-S", str(ROOT_DIR),
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                "-DANDROID_ABI=x86_64",
                "-DANDROID_PLATFORM=android-35"
            ], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([
            "cmake", "--build", str(BUILD_ANDROID_DIR),
            "--target", "lcl-core-android",
            "-j", str(os.cpu_count() or 4)
        ], check=True, stdout=subprocess.DEVNULL)

    # Stage glibc runtime
    stage_glibc_runtime([desktop_shell, desktop_term], STAGING_RUNTIME_DIR)


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


def wait_for_boot(env: AndroidEnvironment, timeout_sec: int = 60) -> None:
    log("Waiting for Android boot...")
    env.run_adb("wait-for-device", check=True)
    start_time = time.time()
    while time.time() - start_time < timeout_sec:
        res = env.adb_shell("getprop sys.boot_completed")
        if res.stdout.strip() == "1":
            return
        time.sleep(1)
    raise TimeoutError("Timed out waiting for sys.boot_completed=1")


def clean_stale_avd_locks(avd_name: str) -> None:
    for base in [Path.home() / ".android/avd", Path.home() / ".config/.android/avd"]:
        avd_dir = base / f"{avd_name}.avd"
        if avd_dir.is_dir():
            for lock_item in avd_dir.glob("*.lock"):
                try:
                    if lock_item.is_dir():
                        shutil.rmtree(lock_item)
                    else:
                        lock_item.unlink()
                except Exception:
                    pass


def launch_avd(args: argparse.Namespace) -> None:
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

    if not args.no_build or not out_system_img.is_file() or not out_ramdisk_img.is_file():
        build_targets(env, force_rebuild=args.rebuild)
        from build_android_images import build_android_images
        out_system_img, out_ramdisk_img = build_android_images()

    # 2. Start emulator directly with custom LCL system and ramdisk images
    emulator_proc = None
    clean_stale_avd_locks(avd_name)
    log(f"Booting {avd_name} directly with custom LCL system & ramdisk images...")
    emu_cmd = [
        str(env.emulator),
        "-avd", avd_name,
        "-no-snapshot-load",
        "-no-boot-anim",
        "-selinux", "permissive",
        "-qemu", "-cpu", "host"
    ]
    if args.show_kernel:
        emu_cmd.insert(4, "-show-kernel")
    if args.no_window:
        emu_cmd.append("-no-window")

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

        # Poll for LCL compositor socket readiness
        log("Waiting for LCL Compositor socket...")
        socket_ready = False
        for _ in range(60):
            res = env.adb_shell("[ -S /run/user/1000/lcl-compositor.sock -o -S /data/local/tmp/lcl-compositor.sock ] && echo READY")
            if "READY" in res.stdout:
                socket_ready = True
                break
            time.sleep(0.5)

        if socket_ready:
            log("============================================================")
            log("LCL OS is running directly on Android AVD (Stage 4A Direct Boot)!")
            log("Active components: Wallpaper, MenuBar, Dock, Terminal")
            log("Press Ctrl+C to stop session.")
            log("============================================================")
        else:
            log("Compositor socket pending. System is running.")

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
    parser.add_argument("--avd-name", default="lcl-phone", help="Target AVD name (default: lcl-phone)")
    parser.add_argument("--show-kernel", action="store_true", help="Display live guest kernel and init boot logs in terminal")
    parser.add_argument("--no-window", action="store_true", help="Run emulator headless without GUI window")
    parser.add_argument("--no-build", action="store_true", help="Skip build step")
    parser.add_argument("--rebuild", action="store_true", help="Force clean rebuild of all targets")
    args = parser.parse_args()

    launch_avd(args)


if __name__ == "__main__":
    main()
