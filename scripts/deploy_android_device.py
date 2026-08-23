#!/usr/bin/env python3
"""
LCL OS - Real Android Device Deployment Script
Deploys lcl-core-android to a rooted Android device via ADB.

Workflow:
  1. Push lcl-core-android and the optional HIDL bridge to /data/local/tmp/
  2. Stop Android System UI (launcher takeover mode)
  3. Launch lcl-core-android as root (Composer3 AIDL / Composer 2.2-2.4 HIDL)
  4. Restore System UI on exit / Ctrl+C

Usage:
  python3 scripts/deploy_android_device.py --rootfs
  python3 scripts/deploy_android_device.py --push-rootfs
  python3 scripts/deploy_android_device.py --restore-only

Requirements:
  - ADB connected device (USB or network)
  - Root access via `adb shell su -c` (Magisk/KernelSU/SuperSU)
  - lcl-core-android ARM64 binary built in build-android-arm64/
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

ROOT_DIR = Path(__file__).resolve().parent.parent
BUILD_ANDROID_ARM64_DIR = ROOT_DIR / "build-android-arm64"
BINARY_NAME = "lcl-core-android"
HIDL_BRIDGE_NAME = "liblcl-android-hidl-bridge.so"
DEVICE_TMP_DIR = "/data/local/tmp"
DEVICE_BINARY_PATH = f"{DEVICE_TMP_DIR}/{BINARY_NAME}"
DEVICE_HIDL_BRIDGE_PATH = f"{DEVICE_TMP_DIR}/{HIDL_BRIDGE_NAME}"
DEVICE_LOG_PATH = f"{DEVICE_TMP_DIR}/lcl-core.log"
DEVICE_RUNTIME_DIR = f"{DEVICE_TMP_DIR}/lcl-runtime"
DEVICE_COMPOSITOR_SOCKET = f"{DEVICE_RUNTIME_DIR}/lcl-compositor.sock"
DEVICE_SESSION_SOCKET = f"{DEVICE_RUNTIME_DIR}/lcl-sessiond.sock"
ROOTFS_IMAGE = ROOT_DIR / "build" / "rootfs" / "lcl-rootfs-aarch64.ext4"
ROOTFS_ARCHIVE = ROOT_DIR / "build" / "rootfs" / "lcl-rootfs-aarch64.ext4.zst"
ROOTFS_SESSION_LAUNCHER = ROOT_DIR / "scripts" / "lcl_android_rootfs_session.sh"
DEVICE_ROOTFS_IMAGE = f"{DEVICE_TMP_DIR}/lcl-rootfs-aarch64.ext4"
DEVICE_ROOTFS_ARCHIVE = f"{DEVICE_ROOTFS_IMAGE}.zst"
DEVICE_ROOTFS_HASH = f"{DEVICE_ROOTFS_IMAGE}.sha256"
DEVICE_ROOTFS_MOUNT = f"{DEVICE_TMP_DIR}/lcl-rootfs"
DEVICE_ROOTFS_LOOP = f"{DEVICE_TMP_DIR}/lcl-rootfs.loop"
DEVICE_SESSION_LAUNCHER = f"{DEVICE_TMP_DIR}/lcl-android-rootfs-session.sh"

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


def stop_rootfs_session() -> None:
    """Stop canonical userspace processes before unmounting its rootfs."""
    process_names = (
        "lcl-desktop-shell", "lcl-sessiond", "lcl-terminal", "lcl-open",
        "lcl-js", "lcl_ui_demo",
    )
    pids: list[str] = []
    for name in process_names:
        result = adb_shell(f"pidof {name}", as_root=True)
        pids.extend(value for value in result.stdout.split() if value.isdigit())
    pids = list(dict.fromkeys(pids))
    if not pids:
        return
    log("Stopping canonical rootfs session...")
    adb_shell("kill -TERM " + " ".join(pids), as_root=True)
    time.sleep(0.7)
    remaining: list[str] = []
    for name in process_names:
        result = adb_shell(f"pidof {name}", as_root=True)
        remaining.extend(value for value in result.stdout.split() if value.isdigit())
    remaining = list(dict.fromkeys(remaining))
    if remaining:
        adb_shell("kill -KILL " + " ".join(remaining), as_root=True)


def unmount_rootfs() -> None:
    """Unmount canonical rootfs bind mounts and release its loop device."""
    stop_rootfs_session()
    for relative in ("dev/pts", "Runtime", "proc", "sys", "dev"):
        adb_shell(f"umount {DEVICE_ROOTFS_MOUNT}/{relative}", as_root=True)
    adb_shell(f"umount {DEVICE_ROOTFS_MOUNT}", as_root=True)
    adb_shell(
        f"if [ -f {DEVICE_ROOTFS_LOOP} ]; then "
        f"loop=$(cat {DEVICE_ROOTFS_LOOP}); "
        f"[ -n \"$loop\" ] && losetup -d \"$loop\" 2>/dev/null; "
        f"rm -f {DEVICE_ROOTFS_LOOP}; fi",
        as_root=True,
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_rootfs_image() -> None:
    """Build the canonical ARM64 glibc rootfs when it is absent."""
    if ROOTFS_IMAGE.is_file():
        return
    log("Canonical ARM64 rootfs is missing; building it in the ARM64 Docker builder...")
    subprocess.run(
        [sys.executable, str(ROOT_DIR / "scripts" / "build_rootfs.py"),
         "--arch", "aarch64", "--size", "1024"],
        cwd=ROOT_DIR,
        check=True,
    )


def push_rootfs_image(force: bool = False) -> None:
    """Compress and deploy the canonical ARM64 ext4 artifact."""
    ensure_rootfs_image()
    rootfs_hash = sha256_file(ROOTFS_IMAGE)
    remote_hash = adb_shell(f"cat {DEVICE_ROOTFS_HASH}", as_root=True).stdout.strip()
    remote_image = adb_shell(f"test -f {DEVICE_ROOTFS_IMAGE}; echo $?", as_root=True).stdout.strip()
    if not force and remote_hash == rootfs_hash and remote_image.endswith("0"):
        log("Canonical ARM64 rootfs already matches the connected phone.")
        return

    zstd = shutil.which("zstd")
    if not zstd:
        raise RuntimeError("Host zstd executable is required to deploy the rootfs image.")
    if (not ROOTFS_ARCHIVE.is_file() or
            ROOTFS_ARCHIVE.stat().st_mtime < ROOTFS_IMAGE.stat().st_mtime):
        log("Compressing canonical ARM64 rootfs with zstd...")
        subprocess.run(
            [zstd, "-T0", "-3", "-f", str(ROOTFS_IMAGE), "-o", str(ROOTFS_ARCHIVE)],
            check=True,
        )

    unmount_rootfs()
    archive_mb = ROOTFS_ARCHIVE.stat().st_size / (1024 * 1024)
    log(f"Pushing compressed rootfs ({archive_mb:.1f} MB)...")
    run_adb("push", str(ROOTFS_ARCHIVE), DEVICE_ROOTFS_ARCHIVE)
    log("Decompressing rootfs on the phone...")
    result = adb_shell(
        f"/system_ext/bin/zstd -d -f {DEVICE_ROOTFS_ARCHIVE} -o {DEVICE_ROOTFS_IMAGE} && "
        f"chmod 0600 {DEVICE_ROOTFS_IMAGE} && rm -f {DEVICE_ROOTFS_ARCHIVE}",
        as_root=True,
    )
    if result.returncode != 0:
        raise RuntimeError("Rootfs decompression failed: " + result.stderr.strip())
    adb_shell(f"echo {rootfs_hash} > {DEVICE_ROOTFS_HASH}", as_root=True)
    log(f"Canonical rootfs deployed: {DEVICE_ROOTFS_IMAGE}")


def mount_rootfs() -> None:
    """Loop-mount rootfs and bind Android kernel/runtime views into it."""
    adb_shell(f"mkdir -p {DEVICE_ROOTFS_MOUNT}", as_root=True)
    mounted = adb_shell(
        f"test -x {DEVICE_ROOTFS_MOUNT}/System/Core/lcl-sessiond; echo $?",
        as_root=True,
    ).stdout.strip()
    if not mounted.endswith("0"):
        unmount_rootfs()
        result = adb_shell(
            f"mkdir -p {DEVICE_ROOTFS_MOUNT}; "
            f"loop=$(losetup -f); "
            f"[ -n \"$loop\" ] && losetup \"$loop\" {DEVICE_ROOTFS_IMAGE} && "
            f"echo \"$loop\" > {DEVICE_ROOTFS_LOOP} && "
            f"mount -t ext4 -o rw,noatime \"$loop\" {DEVICE_ROOTFS_MOUNT}",
            as_root=True,
        )
        if result.returncode != 0:
            raise RuntimeError("Rootfs loop mount failed: " + result.stderr.strip())

    adb_shell(
        f"mkdir -p {DEVICE_ROOTFS_MOUNT}/dev/pts {DEVICE_ROOTFS_MOUNT}/proc "
        f"{DEVICE_ROOTFS_MOUNT}/sys {DEVICE_ROOTFS_MOUNT}/Runtime",
        as_root=True,
    )
    binds = (
        ("/dev", "dev"),
        ("/dev/pts", "dev/pts"),
        ("/proc", "proc"),
        ("/sys", "sys"),
        (f"{DEVICE_TMP_DIR}/lcl-runtime", "Runtime"),
    )
    for source, relative in binds:
        destination = f"{DEVICE_ROOTFS_MOUNT}/{relative}"
        result = adb_shell(
            f"mount | grep -q \" {destination} \" || mount --bind {source} {destination}",
            as_root=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"Rootfs bind mount failed ({source} -> {destination}): " +
                result.stderr.strip()
            )

    probe = adb_shell(
        f"chroot {DEVICE_ROOTFS_MOUNT} /System/Tools/bash -c \""
        f"test -S /Runtime/lcl-compositor.sock -o -d /Runtime; "
        f"echo ROOTFS_ARCH=\\$(/System/Tools/uname -m)\"",
        as_root=True,
    )
    if probe.returncode != 0 or "ROOTFS_ARCH=aarch64" not in probe.stdout:
        raise RuntimeError("ARM64 rootfs chroot probe failed: " +
                           (probe.stderr.strip() or probe.stdout.strip()))
    log("Canonical ARM64 rootfs mounted and chroot probe passed.")


def push_rootfs_session_launcher() -> None:
    if not ROOTFS_SESSION_LAUNCHER.is_file():
        raise RuntimeError(f"Rootfs session launcher missing: {ROOTFS_SESSION_LAUNCHER}")
    run_adb("push", str(ROOTFS_SESSION_LAUNCHER), DEVICE_SESSION_LAUNCHER)
    adb_shell(f"chmod 0755 {DEVICE_SESSION_LAUNCHER}", as_root=True)


def prepare_runtime_for_launch() -> None:
    """Reject overlapping sessions and remove sockets left by dead processes."""
    process_names = ("lcl-core-android", "lcl-sessiond", "lcl-desktop-shell", "lcl-terminal")
    active: list[str] = []
    for name in process_names:
        result = adb_shell(f"pidof {name}", as_root=True)
        pids = [value for value in result.stdout.split() if value.isdigit()]
        if pids:
            active.append(f"{name} ({', '.join(pids)})")
    if active:
        raise RuntimeError(
            "An LCL session is already running on the phone: " + ", ".join(active) +
            ". Stop its current launcher with Ctrl+C or run './main.py android --restore-only'."
        )

    result = adb_shell(
        f"rm -f {DEVICE_COMPOSITOR_SOCKET} {DEVICE_SESSION_SOCKET}",
        as_root=True,
    )
    if result.returncode != 0:
        raise RuntimeError("Could not remove stale LCL runtime sockets: " + result.stderr.strip())
    log("Stale runtime sockets cleared; no overlapping LCL session found.")


def wait_for_compositor_socket(
    timeout_seconds: float = 8.0,
    process: subprocess.Popen | None = None,
) -> bool:
    """Wait until the kernel reports the compositor socket as listening."""
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            return False
        result = adb_shell(
            f"grep -F {DEVICE_COMPOSITOR_SOCKET} /proc/net/unix | "
            f"grep -q \"00010000 0005 01\"",
            as_root=True,
        )
        if result.returncode == 0:
            return True
        time.sleep(0.1)
    return False


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
    adb_shell(f"mkdir -p {DEVICE_RUNTIME_DIR} && chmod 700 {DEVICE_RUNTIME_DIR}", as_root=True)
    log(f"Runtime directory ready: {DEVICE_RUNTIME_DIR}")
    return DEVICE_RUNTIME_DIR


def launch_lcl(no_stop_sysui: bool = False, logcat: bool = False,
               use_rootfs: bool = False, force_rootfs: bool = False) -> None:
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

    # 4. Reject an overlapping session before changing deployed artifacts.
    runtime_dir = setup_runtime_dir()
    prepare_runtime_for_launch()

    # 4b. Push binary
    push_binary()

    # 4c. Deploy and mount the canonical ARM64 glibc userspace when requested.
    if use_rootfs:
        try:
            push_rootfs_image(force=force_rootfs)
            mount_rootfs()
            push_rootfs_session_launcher()
        except Exception:
            # These steps run before the main session try/finally. Do not leave
            # loop or bind mounts behind when rootfs preparation fails early.
            unmount_rootfs()
            raise

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
    session_proc = None

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

        if use_rootfs:
            log("Waiting for Android compositor IPC before starting canonical userspace...")
            if not wait_for_compositor_socket(process=lcl_proc):
                raise RuntimeError(
                    "LCL compositor did not expose a live listening IPC socket "
                    "before the rootfs session timeout."
                )
            session_proc = subprocess.Popen([
                "adb", "shell",
                f"su -c '{DEVICE_SESSION_LAUNCHER} 2>&1'",
            ])
            log("Canonical ARM64 rootfs session launched.")

        lcl_proc.wait()

    except KeyboardInterrupt:
        print("")
        log("Interrupt received. Terminating LCL session...")
        if lcl_proc and lcl_proc.poll() is None:
            lcl_proc.terminate()

    finally:
        if use_rootfs:
            stop_rootfs_session()
            if session_proc is not None and session_proc.poll() is None:
                session_proc.terminate()
        stop_lcl_process()

        if use_rootfs:
            unmount_rootfs()

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
    stop_rootfs_session()
    stop_lcl_process()
    unmount_rootfs()
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
    parser.add_argument(
        "--rootfs",
        action="store_true",
        help="Mount and launch the canonical ARM64 glibc rootfs desktop session"
    )
    parser.add_argument(
        "--push-rootfs",
        action="store_true",
        help="Force re-upload of the canonical ARM64 rootfs (implies --rootfs)"
    )
    args = parser.parse_args()

    if args.restore_only:
        restore_only()
        return

    launch_lcl(
        no_stop_sysui=args.no_stop_sysui,
        logcat=args.logcat,
        use_rootfs=args.rootfs or args.push_rootfs,
        force_rootfs=args.push_rootfs,
    )


if __name__ == "__main__":
    main()
