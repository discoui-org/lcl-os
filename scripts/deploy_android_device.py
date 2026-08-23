#!/usr/bin/env python3
"""
LCL OS - Real Android Device Deployment Script
Deploys lcl-core-android to a rooted Android device via ADB.

Workflow:
  1. Push lcl-core-android and the optional HIDL bridge to /data/local/tmp/
  2. Stop Android System UI (launcher takeover mode)
  3. Launch lcl-core-android as root (Composer3 AIDL / Composer 2.4 HIDL)
  4. Restore System UI on exit / Ctrl+C

Usage:
  python3 scripts/deploy_android_device.py [--restore-only] [--no-stop-sysui] [--logcat]

Requirements:
  - ADB connected device (USB or network)
  - Root access via `adb shell su -c` (Magisk/KernelSU/SuperSU)
  - lcl-core-android ARM64 binary built in build-android-arm64/
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_ANDROID_ARM64_DIR = ROOT_DIR / "build-android-arm64"
BINARY_NAME = "lcl-core-android"
HIDL_BRIDGE_NAME = "liblcl-android-hidl-bridge.so"
DEVICE_TMP_DIR = "/data/local/tmp"
DEVICE_BINARY_PATH = f"{DEVICE_TMP_DIR}/{BINARY_NAME}"
DEVICE_HIDL_BRIDGE_PATH = f"{DEVICE_TMP_DIR}/{HIDL_BRIDGE_NAME}"
DEVICE_LOG_PATH = f"{DEVICE_TMP_DIR}/lcl-core.log"

# Android system services to suspend for display takeover
SYSUI_PACKAGE = "com.android.systemui"
LAUNCHER_PACKAGES = [
    "com.android.launcher3",
    "com.google.android.apps.nexuslauncher",
    "com.miui.home",
    "com.sec.android.app.launcher",
    "org.lineageos.trebuchet",
    "com.crdroid.launcher",
]


def log(msg: str) -> None:
    print(f"[LCL DEPLOY] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL DEPLOY ERROR] {msg}", file=sys.stderr, flush=True)


def run_adb(*args: str, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess:
    cmd = ["adb"] + list(args)
    if capture:
        return subprocess.run(cmd, capture_output=True, text=True, check=check)
    return subprocess.run(cmd, check=check)


def adb_shell(cmd: str, as_root: bool = True, capture: bool = True, check: bool = False) -> subprocess.CompletedProcess:
    if as_root:
        shell_cmd = f"su -c '{cmd}'"
    else:
        shell_cmd = cmd
    return subprocess.run(["adb", "shell", shell_cmd], capture_output=capture, text=True, check=check)


def check_device() -> str:
    """Check ADB device is connected and authorized. Returns device serial."""
    res = run_adb("devices", capture=True, check=False)
    lines = res.stdout.strip().splitlines()
    devices = []
    for line in lines[1:]:
        parts = line.strip().split()
        if len(parts) >= 2:
            serial, state = parts[0], parts[1]
            if state == "device":
                devices.append(serial)
            elif state == "unauthorized":
                err(f"Device {serial} is UNAUTHORIZED.")
                err("  -> On your phone: tap 'Allow USB debugging' in the notification/dialog.")
                err("  -> Then re-run this script.")
                sys.exit(1)
            elif state == "offline":
                err(f"Device {serial} is OFFLINE. Try: adb kill-server && adb start-server")
                sys.exit(1)

    if not devices:
        err("No ADB devices found.")
        err("  -> Connect your phone via USB")
        err("  -> Enable USB Debugging in Developer Options")
        sys.exit(1)

    serial = devices[0]
    log(f"Connected to device: {serial}")
    return serial


def check_root() -> bool:
    """Verify root access via su."""
    log("Checking root access...")
    res = adb_shell("id", as_root=True, capture=True)
    if "uid=0" in res.stdout:
        log("Root access confirmed (uid=0).")
        return True
    else:
        err("Root access NOT available. Output: " + res.stdout.strip())
        err("  -> Make sure Magisk/KernelSU is installed and 'adb' is granted root in Magisk app.")
        return False


def get_device_info() -> dict:
    """Fetch basic device info for compatibility check."""
    def prop(name: str) -> str:
        res = adb_shell(f"getprop {name}", as_root=False)
        return res.stdout.strip()

    return {
        "model": prop("ro.product.model"),
        "android_version": prop("ro.build.version.release"),
        "sdk_version": prop("ro.build.version.sdk"),
        "abi": prop("ro.product.cpu.abi"),
        "build": prop("ro.build.display.id"),
    }


def stop_system_ui() -> None:
    """Stop Android System UI, launchers, and SurfaceFlinger for display takeover."""
    log("Stopping System UI and SurfaceFlinger for display takeover...")

    # Stop SystemUI app
    adb_shell(f"am force-stop {SYSUI_PACKAGE}", as_root=True)
    time.sleep(0.3)

    # Stop known launchers
    for pkg in LAUNCHER_PACKAGES:
        res = adb_shell(f"pm list packages | grep {pkg}", as_root=False)
        if pkg in res.stdout:
            log(f"  Stopping launcher: {pkg}")
            adb_shell(f"am force-stop {pkg}", as_root=True)

    # Enable full immersive mode to suppress system bar auto-restore
    adb_shell("settings put global policy_control immersive.full=*", as_root=False)

    # Stop SurfaceFlinger via Android init — this releases the HWComposer/Composer HAL
    # client so lcl-core-android can connect to it directly.
    log("  Stopping SurfaceFlinger (releases Composer HAL exclusive lock)...")
    adb_shell("stop surfaceflinger", as_root=True)
    time.sleep(1.5)  # Give SurfaceFlinger time to fully release HAL

    log("SurfaceFlinger and System UI stopped. Composer HAL is now available.")


def restore_system_ui() -> None:
    """Restore SurfaceFlinger and Android System UI after session."""
    log("Restoring SurfaceFlinger and System UI...")

    # Restart SurfaceFlinger first
    adb_shell("start surfaceflinger", as_root=True)
    time.sleep(2)

    # Restart SystemUI
    adb_shell(f"am start-service {SYSUI_PACKAGE}/.SystemUIService", as_root=True)
    time.sleep(0.5)

    # Broadcast boot completed to trigger full UI restart
    adb_shell("am broadcast -a android.intent.action.BOOT_COMPLETED", as_root=True)

    # Clear immersive policy
    adb_shell("settings delete global policy_control", as_root=False)

    log("System UI restored.")


def stop_lcl_process() -> None:
    """Stop only LCL Android processes left behind by a detached ADB shell."""
    result = adb_shell("pidof lcl-core-android", as_root=True)
    pids = [value for value in result.stdout.split() if value.isdigit()]
    if not pids:
        return
    log("Stopping remote LCL compositor...")
    adb_shell("kill -TERM " + " ".join(pids), as_root=True)
    time.sleep(0.5)
    remaining = adb_shell("pidof lcl-core-android", as_root=True).stdout.split()
    remaining = [value for value in remaining if value.isdigit()]
    if remaining:
        adb_shell("kill -KILL " + " ".join(remaining), as_root=True)


def push_binary() -> None:
    """Push lcl-core-android and its optional HIDL ABI bridge to device."""
    binary = BUILD_ANDROID_ARM64_DIR / BINARY_NAME
    if not binary.is_file():
        err(f"Binary not found: {binary}")
        err("  -> Build first: cd build-android-arm64 && cmake --build . --target lcl-core-android")
        sys.exit(1)

    size_mb = binary.stat().st_size / (1024 * 1024)
    log(f"Pushing {BINARY_NAME} ({size_mb:.1f} MB) to {DEVICE_BINARY_PATH}...")

    run_adb("push", str(binary), DEVICE_TMP_DIR)
    adb_shell(f"chmod 755 {DEVICE_BINARY_PATH}", as_root=True)
    bridge = BUILD_ANDROID_ARM64_DIR / HIDL_BRIDGE_NAME
    if bridge.is_file():
        bridge_size_mb = bridge.stat().st_size / (1024 * 1024)
        log(f"Pushing HIDL bridge ({bridge_size_mb:.1f} MB) to {DEVICE_HIDL_BRIDGE_PATH}...")
        run_adb("push", str(bridge), DEVICE_TMP_DIR)
        adb_shell(f"chmod 755 {DEVICE_HIDL_BRIDGE_PATH}", as_root=True)
    else:
        log("HIDL bridge not present; this deployment will support Composer3 AIDL only.")
    log("Android runtime files pushed and marked executable.")


def setup_runtime_dir() -> str:
    """Create writable runtime directory on device for LCL sockets."""
    runtime_dir = f"{DEVICE_TMP_DIR}/lcl-runtime"
    adb_shell(f"mkdir -p {runtime_dir} && chmod 700 {runtime_dir}", as_root=True)
    log(f"Runtime directory ready: {runtime_dir}")
    return runtime_dir


def launch_lcl(no_stop_sysui: bool = False, logcat: bool = False) -> None:
    """Main deployment logic: push binary, stop SysUI, launch LCL compositor."""

    log("=" * 60)
    log("  LCL Core Linux -- Real Android Device Deployment")
    log("  Stage 4C.3 | Android Composer Direct Display Takeover")
    log("=" * 60)

    # 1. Check connectivity
    check_device()

    # 2. Verify root
    if not check_root():
        err("Cannot continue without root access.")
        sys.exit(1)

    # 3. Device info
    info = get_device_info()
    log(f"Device: {info['model']} | Android {info['android_version']} (SDK {info['sdk_version']}) | ABI: {info['abi']}")

    if info["abi"] not in ("arm64-v8a", "aarch64"):
        err(f"Device ABI '{info['abi']}' is not arm64-v8a. This binary requires ARM64.")
        sys.exit(1)

    # 4. Push binary
    push_binary()

    # 4b. Create runtime directory on device
    runtime_dir = setup_runtime_dir()

    # 5. Stop System UI for display takeover (unless suppressed)
    if not no_stop_sysui:
        stop_system_ui()
        time.sleep(1)
    else:
        log("Skipping System UI stop (--no-stop-sysui mode).")

    # 6. Launch lcl-core-android as root
    log("Launching lcl-core-android compositor on device...")
    log("  The LCL desktop should appear on your phone screen.")
    log("  Press Ctrl+C here to terminate and restore System UI.")
    log("-" * 60)

    lc_proc = None
    lcl_proc = None

    try:
        if logcat:
            lc_proc = subprocess.Popen(
                ["adb", "logcat", "-s", "LCL:V", "lcl:V", "*:W"],
                stdout=None, stderr=None
            )

        # Set LCL_RUNTIME_DIR so lcl-core-android uses the writable path
        # ANDROID_DATA=/data is required for linker and binder initialization
        launch_env = (
            f"LCL_RUNTIME_DIR={runtime_dir} ANDROID_DATA=/data "
            f"LD_LIBRARY_PATH={DEVICE_TMP_DIR}:/system/lib64:/vendor/lib64:/system_ext/lib64"
        )
        lcl_proc = subprocess.Popen([
            "adb", "shell",
            f"su -c '{launch_env} {DEVICE_BINARY_PATH} 2>&1 | tee {DEVICE_LOG_PATH}'"
        ])

        lcl_proc.wait()

    except KeyboardInterrupt:
        print("")
        log("Interrupt received. Terminating LCL session...")
        if lcl_proc and lcl_proc.poll() is None:
            lcl_proc.terminate()

    finally:
        stop_lcl_process()

        # Always restore System UI
        if not no_stop_sysui:
            restore_system_ui()

        if lc_proc is not None:
            lc_proc.terminate()

        log("Cleanup complete. LCL session ended.")


def restore_only() -> None:
    """Just restore System UI without launching LCL."""
    log("Restore-only mode: Restarting Android System UI...")
    check_device()
    restore_system_ui()
    log("Done.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="LCL OS - Real Android Device Deployment (ADB + Root)"
    )
    parser.add_argument(
        "--restore-only",
        action="store_true",
        help="Only restore System UI (use if LCL crashed without cleanup)"
    )
    parser.add_argument(
        "--no-stop-sysui",
        action="store_true",
        help="Don't stop System UI (overlay test mode)"
    )
    parser.add_argument(
        "--logcat",
        action="store_true",
        help="Show Android logcat output alongside LCL compositor logs"
    )
    args = parser.parse_args()

    if args.restore_only:
        restore_only()
        return

    launch_lcl(
        no_stop_sysui=args.no_stop_sysui,
        logcat=args.logcat,
    )


if __name__ == "__main__":
    main()
