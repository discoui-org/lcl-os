#!/usr/bin/env python3
"""
LCL OS - Android ADB Deployment Script
Deploys lcl-core-android to a rooted physical device or running emulator via ADB.

Workflow:
  1. Push lcl-core-android and the optional HIDL bridge to /data/local/tmp/
  2. Stop Android System UI (launcher takeover mode)
  3. Launch lcl-core-android as root (Composer3 AIDL / Composer 2.2-2.4 HIDL)
  4. Restore System UI on exit / Ctrl+C

Usage:
  python3 tooling/deploy/deploy_android_device.py --rootfs
  python3 tooling/deploy/deploy_android_device.py --push-rootfs
  python3 tooling/deploy/deploy_android_device.py --restore-only

Requirements:
  - ADB connected device (USB or network)
  - Root adbd (`adb root`) or working `su -c` access
  - lcl-core-android matching the connected target ABI
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tooling.paths import ANDROID_ZSTD_BINARY as ARM64_ZSTD_BINARY
from tooling.paths import ROOTFS_DIR, android_build_dir

DEVICE_GESTALT_DIR = ROOT_DIR / "config" / "devices"
TARGET_ARCH = "aarch64"
BUILD_ANDROID_DIR = android_build_dir("arm64-v8a")
BINARY_NAME = "lcl-core-android"
RASTERD_NAME = "lcl-rasterd-android"
HIDL_BRIDGE_NAME = "liblcl-android-hidl-bridge.so"
DEVICE_TMP_DIR = "/data/local/tmp"
DEVICE_BINARY_PATH = f"{DEVICE_TMP_DIR}/{BINARY_NAME}"
DEVICE_RASTERD_PATH = f"{DEVICE_TMP_DIR}/{RASTERD_NAME}"
DEVICE_HIDL_BRIDGE_PATH = f"{DEVICE_TMP_DIR}/{HIDL_BRIDGE_NAME}"
DEVICE_LOG_PATH = f"{DEVICE_TMP_DIR}/lcl-core.log"
DEVICE_RUNTIME_DIR = f"{DEVICE_TMP_DIR}/lcl-runtime"
DEVICE_FONT_DIR = f"{DEVICE_RUNTIME_DIR}/fonts"
DEVICE_GESTALT_UPLOAD_PATH = f"{DEVICE_TMP_DIR}/lcl-gestalt.json.upload"
DEVICE_GESTALT_PATH = f"{DEVICE_RUNTIME_DIR}/gestalt.json"
DEVICE_COMPOSITOR_SOCKET = f"{DEVICE_RUNTIME_DIR}/lcl-compositor.sock"
DEVICE_SESSION_SOCKET = f"{DEVICE_RUNTIME_DIR}/lcl-sessiond.sock"
ROOTFS_IMAGE = ROOTFS_DIR / "lcl-rootfs-aarch64.ext4"
ROOTFS_ARCHIVE = ROOTFS_DIR / "lcl-rootfs-aarch64.ext4.zst"
ROOTFS_SESSION_LAUNCHER = Path(__file__).resolve().parent / "lcl_android_rootfs_session.sh"
DEVICE_ROOTFS_IMAGE = f"{DEVICE_TMP_DIR}/lcl-rootfs-aarch64.ext4"
DEVICE_ROOTFS_ARCHIVE = f"{DEVICE_ROOTFS_IMAGE}.zst"
DEVICE_ROOTFS_HASH = f"{DEVICE_ROOTFS_IMAGE}.sha256"
ANDROID_ZSTD_BINARY: Path | None = ARM64_ZSTD_BINARY
DEVICE_ZSTD_BINARY = f"{DEVICE_TMP_DIR}/lcl-zstd"
DEVICE_ROOTFS_MOUNT = f"{DEVICE_TMP_DIR}/lcl-rootfs"
DEVICE_ROOTFS_LOOP = f"{DEVICE_TMP_DIR}/lcl-rootfs.loop"
DEVICE_SESSION_LAUNCHER = f"{DEVICE_TMP_DIR}/lcl-android-rootfs-session.sh"
NATIVE_CLIENT_WRAPPER = Path(__file__).resolve().parent / "lcl_android_native_client_wrapper.sh"
DEVICE_NATIVE_CLIENT_DIR = f"{DEVICE_TMP_DIR}/lcl-native-clients"
DEVICE_NATIVE_CLIENT_WRAPPER = f"{DEVICE_NATIVE_CLIENT_DIR}/launch"
NATIVE_CLIENT_ARTIFACTS = {
    "lcl-desktop-shell": Path("lcl-desktop-shell"),
    "lcl-mobile-shell": Path("lcl-mobile-shell"),
    "lcl-terminal": Path("lcl-terminal"),
    "lcl_ui_demo": Path("apps/ui_demo/lcl_ui_demo"),
    "lcl-js": Path("lcl-js"),
}
NATIVE_CLIENT_BINARIES = tuple(NATIVE_CLIENT_ARTIFACTS)
ANDROID_NATIVE_LD_LIBRARY_PATH = (
    "/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:"
    "/system/lib64:/vendor/lib64:/system_ext/lib64"
)
ROOT_SHELL_MODE: str | None = None
RUNTIME_FONTS = {
    "Inter-Regular.otf": ROOT_DIR / "assets" / "fonts" / "inter" / "Inter-Regular.otf",
    "JetBrainsMono-Regular.ttf": (
        ROOT_DIR / "assets" / "fonts" / "jetbrains-mono" / "JetBrainsMono-Regular.ttf"
    ),
}

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


def adb_shell_command(cmd: str, as_root: bool = True) -> list[str]:
    """Build an adb shell command using the root transport detected at startup."""
    if not as_root or ROOT_SHELL_MODE == "adbd":
        shell_cmd = cmd
    else:
        shell_cmd = f"su -c {shlex.quote(cmd)}"
    return ["adb", "shell", shell_cmd]


def adb_shell(cmd: str, as_root: bool = True, capture: bool = True, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        adb_shell_command(cmd, as_root=as_root),
        capture_output=capture,
        text=True,
        check=check,
    )


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
    """Select direct root adbd when available, otherwise fall back to su."""
    global ROOT_SHELL_MODE
    log("Checking root access...")

    direct = subprocess.run(
        ["adb", "shell", "id"], capture_output=True, text=True, check=False,
    )
    if "uid=0" in direct.stdout:
        ROOT_SHELL_MODE = "adbd"
        log("Root access confirmed through adbd (uid=0).")
        ensure_permissive_selinux()
        return True

    root_attempt = run_adb("root", capture=True, check=False)
    run_adb("wait-for-device", capture=True, check=False)
    direct = subprocess.run(
        ["adb", "shell", "id"], capture_output=True, text=True, check=False,
    )
    if "uid=0" in direct.stdout:
        ROOT_SHELL_MODE = "adbd"
        log("Root access confirmed after adb root (uid=0).")
        ensure_permissive_selinux()
        return True

    via_su = subprocess.run(
        ["adb", "shell", f"su -c {shlex.quote('id')}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if "uid=0" in via_su.stdout:
        ROOT_SHELL_MODE = "su"
        log("Root access confirmed through su (uid=0).")
        ensure_permissive_selinux()
        return True

    details = " | ".join(
        value for value in (
            direct.stdout.strip(), direct.stderr.strip(),
            root_attempt.stdout.strip(), root_attempt.stderr.strip(),
            via_su.stdout.strip(), via_su.stderr.strip(),
        ) if value
    )
    err("Root access NOT available. Output: " + details)
    err("  -> This target needs either root adbd or a working su implementation.")
    return False


def ensure_permissive_selinux() -> None:
    """Ensure SELinux is permissive so kernel loop workers and chroot mounts can operate."""
    status = adb_shell("getenforce", as_root=True)
    if status.stdout.strip().lower() == "enforcing":
        adb_shell("setenforce 0", as_root=True)
        log("SELinux set to Permissive mode for loop-mount and runtime isolation.")


def configure_target_for_abi(abi: str) -> None:
    """Select existing Android and canonical rootfs artifacts for one ADB ABI."""
    global TARGET_ARCH, BUILD_ANDROID_DIR, ROOTFS_IMAGE, ROOTFS_ARCHIVE
    global DEVICE_ROOTFS_IMAGE, DEVICE_ROOTFS_ARCHIVE, DEVICE_ROOTFS_HASH
    global ANDROID_ZSTD_BINARY

    if abi in ("arm64-v8a", "aarch64"):
        TARGET_ARCH = "aarch64"
        BUILD_ANDROID_DIR = android_build_dir("arm64-v8a")
        ANDROID_ZSTD_BINARY = ARM64_ZSTD_BINARY
    elif abi in ("x86_64", "amd64"):
        TARGET_ARCH = "x86_64"
        BUILD_ANDROID_DIR = android_build_dir("x86_64")
        # No prebuilt Android x86_64 decompressor is required: deployment
        # pushes the existing ext4 artifact directly when it changes.
        ANDROID_ZSTD_BINARY = None
    else:
        raise RuntimeError(
            f"Device ABI '{abi}' is unsupported; expected arm64-v8a or x86_64."
        )

    filename = f"lcl-rootfs-{TARGET_ARCH}.ext4"
    ROOTFS_IMAGE = ROOTFS_DIR / filename
    ROOTFS_ARCHIVE = ROOTFS_IMAGE.with_suffix(ROOTFS_IMAGE.suffix + ".zst")
    DEVICE_ROOTFS_IMAGE = f"{DEVICE_TMP_DIR}/{filename}"
    DEVICE_ROOTFS_ARCHIVE = f"{DEVICE_ROOTFS_IMAGE}.zst"
    DEVICE_ROOTFS_HASH = f"{DEVICE_ROOTFS_IMAGE}.sha256"


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


def device_gestalt_for_model(model: str) -> Path | None:
    """Return the exact config/devices/<ro.product.model>.json match when available."""
    model = model.strip()
    filename = f"{model}.json"
    if not model or Path(filename).name != filename:
        return None
    candidate = DEVICE_GESTALT_DIR / filename
    return candidate if candidate.is_file() else None


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


def rootfs_process_ids() -> list[str]:
    """Return processes whose root, cwd, or executable is inside LCL rootfs."""
    result = adb_shell(
        "ls -l /proc/[0-9]*/root /proc/[0-9]*/cwd "
        "/proc/[0-9]*/exe 2>/dev/null",
        as_root=True,
    )
    prefix = DEVICE_ROOTFS_MOUNT + "/"
    pids: list[str] = []
    for line in result.stdout.splitlines():
        source, separator, target = line.partition(" -> ")
        if not separator:
            continue
        match = re.search(r"/proc/(\d+)/(?:root|cwd|exe)$", source)
        if match is None:
            continue
        if target == DEVICE_ROOTFS_MOUNT or target.startswith(prefix):
            pids.append(match.group(1))
    return list(dict.fromkeys(pids))


def stop_rootfs_session() -> None:
    """Stop canonical userspace processes before unmounting its rootfs."""
    process_names = (
        "lcl-desktop-shell", "lcl-mobile-shell", "lcl-sessiond", "lcl-terminal", "lcl-open",
        "lcl-js", "lcl_ui_demo",
    )
    def active_pids() -> list[str]:
        pids = rootfs_process_ids()
        for name in process_names:
            result = adb_shell(f"pidof {name}", as_root=True)
            pids.extend(value for value in result.stdout.split() if value.isdigit())
        return list(dict.fromkeys(pids))

    pids = active_pids()
    if not pids:
        return
    log("Stopping canonical rootfs session...")
    adb_shell("kill -TERM " + " ".join(pids), as_root=True)
    time.sleep(0.7)
    remaining = active_pids()
    if remaining:
        adb_shell("kill -KILL " + " ".join(remaining), as_root=True)


def rootfs_mount_points() -> list[str]:
    """Return every mount layer rooted at the canonical rootfs path."""
    mounted = adb_shell("mount", as_root=True).stdout.splitlines()
    prefix = DEVICE_ROOTFS_MOUNT + "/"
    mount_points: list[str] = []
    for line in mounted:
        fields = line.split()
        if len(fields) < 3 or fields[1] != "on":
            continue
        if fields[2] == DEVICE_ROOTFS_MOUNT or fields[2].startswith(prefix):
            mount_points.append(fields[2])
    return mount_points


def make_mount_rslave(mount_point: str) -> None:
    """Prevent a rootfs bind mount from propagating changes back to Android."""
    result = adb_shell(
        f"mount -o rslave none {shlex.quote(mount_point)}",
        as_root=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Could not isolate mount propagation for {mount_point}: " +
            (result.stderr.strip() or result.stdout.strip())
        )


def rootfs_loop_devices() -> list[str] | None:
    """Return loops backed by the rootfs; an absent first-deploy image has none."""
    rootfs_image = shlex.quote(DEVICE_ROOTFS_IMAGE)
    result = adb_shell(
        f"if [ ! -e {rootfs_image} ]; then exit 0; fi; "
        f"losetup -j {rootfs_image}",
        as_root=True,
    )
    if result.returncode != 0:
        err("Could not inspect rootfs loop devices: " +
            (result.stderr.strip() or result.stdout.strip()))
        return None

    devices: list[str] = []
    for line in result.stdout.splitlines():
        device = line.partition(":")[0].strip()
        if re.fullmatch(r"/dev/block/loop\d+", device):
            devices.append(device)
    return devices


def detach_rootfs_loops() -> bool:
    """Detach only loop devices backed by LCL's canonical rootfs image."""
    devices = rootfs_loop_devices()
    if devices is None:
        return False

    for device in devices:
        result = adb_shell(f"losetup -d {shlex.quote(device)}", as_root=True)
        if result.returncode != 0:
            err(f"Could not detach stale rootfs loop {device}: " +
                (result.stderr.strip() or result.stdout.strip()))
            return False

    result = adb_shell(f"rm -f {DEVICE_ROOTFS_LOOP}", as_root=True)
    if result.returncode != 0:
        err("Could not remove stale rootfs loop marker: " +
            (result.stderr.strip() or result.stdout.strip()))
        return False
    return True


def usable_loop_devices() -> list[str] | None:
    """Find unconfigured loops that are not held by Android device-mapper."""
    result = adb_shell("ls -1 /sys/class/block", as_root=True)
    if result.returncode != 0:
        err("Could not enumerate Android loop devices: " +
            (result.stderr.strip() or result.stdout.strip()))
        return None

    loop_names = sorted(
        (name for name in result.stdout.splitlines()
         if re.fullmatch(r"loop\d+", name)),
        key=lambda name: int(name[4:]),
    )
    if not loop_names:
        err("Android exposes no loop devices under /sys/class/block.")
        return None

    checks = []
    for name in loop_names:
        sysfs = f"/sys/class/block/{name}"
        device = f"/dev/block/{name}"
        checks.append(
            f"if [ -b {device} ] && "
            f"[ \"$(cat {sysfs}/size 2>/dev/null)\" = 0 ] && "
            f"[ -z \"$(ls -A {sysfs}/holders 2>/dev/null)\" ]; then "
            f"echo {device}; fi"
        )
    result = adb_shell("; ".join(checks), as_root=True)
    if result.returncode != 0:
        err("Could not inspect Android loop-device availability: " +
            (result.stderr.strip() or result.stdout.strip()))
        return None

    allowed = {f"/dev/block/{name}" for name in loop_names}
    return [line.strip() for line in result.stdout.splitlines()
            if line.strip() in allowed]


def loop_device_is_unheld(device: str) -> bool:
    """Return whether a validated loop device has no device-mapper holders."""
    if not re.fullmatch(r"/dev/block/loop\d+", device):
        return False
    name = device.rsplit("/", 1)[-1]
    sysfs = f"/sys/class/block/{name}"
    result = adb_shell(
        f"[ -b {device} ] && [ -z \"$(ls -A {sysfs}/holders 2>/dev/null)\" ]",
        as_root=True,
    )
    return result.returncode == 0


def loop_device_is_usable(device: str) -> bool:
    """Return whether a loop is unconfigured and has no active holders."""
    if not re.fullmatch(r"/dev/block/loop\d+", device):
        return False
    name = device.rsplit("/", 1)[-1]
    sysfs = f"/sys/class/block/{name}"
    result = adb_shell(
        f"[ -b {device} ] && "
        f"[ \"$(cat {sysfs}/size 2>/dev/null)\" = 0 ] && "
        f"[ -z \"$(ls -A {sysfs}/holders 2>/dev/null)\" ]",
        as_root=True,
    )
    return result.returncode == 0


def request_free_loop_device() -> str | None:
    """Ask Android's loop-control driver to expose another free loop."""
    result = adb_shell("losetup -f", as_root=True)
    if result.returncode != 0:
        return None
    device = result.stdout.strip()
    if loop_device_is_usable(device):
        return device
    return None


def record_rootfs_loop(device: str) -> None:
    """Persist the selected rootfs loop for diagnostics and compatibility."""
    marker = adb_shell(
        f"echo {shlex.quote(device)} > {DEVICE_ROOTFS_LOOP}",
        as_root=True,
    )
    if marker.returncode != 0:
        raise RuntimeError(
            "Could not record rootfs loop device: " +
            (marker.stderr.strip() or marker.stdout.strip())
        )


def attach_rootfs_loop() -> str:
    """Attach the rootfs to a genuinely usable loop device."""
    # A detached autoclear loop can remain alive while Android still has an
    # open reference. Reuse it only when it is backed by our exact image and
    # has no device-mapper holders; trying losetup -f would select loop4 on
    # some devices even though Android's com.android.resolv still holds it.
    existing = rootfs_loop_devices()
    if existing is None:
        raise RuntimeError("Could not inspect existing rootfs loop devices.")
    for device in existing:
        if loop_device_is_unheld(device):
            record_rootfs_loop(device)
            return device

    devices = usable_loop_devices()
    if devices is None:
        raise RuntimeError("Could not inspect Android loop devices.")
    if not devices:
        requested = request_free_loop_device()
        if requested is not None:
            devices.append(requested)
        else:
            raise RuntimeError(
                "Android loop-control could not provide an unconfigured loop "
                "device without active holders."
            )

    failures: list[str] = []
    for device in devices:
        result = adb_shell(
            f"losetup {shlex.quote(device)} {shlex.quote(DEVICE_ROOTFS_IMAGE)}",
            as_root=True,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            failures.append(f"{device}: {detail or 'association failed'}")
            continue

        try:
            record_rootfs_loop(device)
            return device
        except RuntimeError:
            adb_shell(f"losetup -d {shlex.quote(device)}", as_root=True)
            raise

    raise RuntimeError(
        "No usable Android loop device accepted the rootfs image: " +
        "; ".join(failures)
    )


def unmount_rootfs() -> bool:
    """Unmount every rootfs mount layer and release its loop device."""
    stop_rootfs_session()
    if rootfs_mount_points():
        # Older deployments created shared recursive binds. Convert the whole
        # subtree before unmounting so teardown cannot propagate back to the
        # phone's live /dev, /system, /vendor, or /apex mount trees.
        result = adb_shell(
            f"mount -o rslave none {DEVICE_ROOTFS_MOUNT}",
            as_root=True,
        )
        if result.returncode != 0:
            err("Could not isolate stale rootfs mount propagation: " +
                (result.stderr.strip() or result.stdout.strip()))
    # Recursive binds (notably /apex and /dev/binderfs) create nested mount
    # points. The same destination may also have multiple stacked bind layers
    # after an interrupted relaunch, so preserve duplicates and keep unwinding
    # while at least one layer can be removed.
    for _ in range(64):
        mount_points = rootfs_mount_points()
        if not mount_points:
            break
        removed_layer = False
        for mount_point in sorted(
                mount_points,
                key=lambda path: (path.count("/"), len(path)),
                reverse=True):
            result = adb_shell(
                f"umount {shlex.quote(mount_point)}",
                as_root=True,
            )
            removed_layer = removed_layer or result.returncode == 0
        if not removed_layer:
            break

    remaining = rootfs_mount_points()
    if remaining:
        err("Could not fully unmount stale rootfs mount(s): " +
            ", ".join(sorted(set(remaining))))
        return False

    # Do not trust the marker alone: after a reboot its loop number may belong
    # to Android. Match the backing image so orphaned LCL loops are also found.
    return detach_rootfs_loops()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_rootfs_image(allow_build: bool = True) -> None:
    """Require or build the canonical glibc rootfs selected for the ADB ABI."""
    if ROOTFS_IMAGE.is_file():
        return
    if not allow_build:
        raise RuntimeError(
            f"--no-build requires the canonical {TARGET_ARCH} rootfs: {ROOTFS_IMAGE}"
        )
    log(f"Canonical {TARGET_ARCH} rootfs is missing; building it in Docker...")
    subprocess.run(
        [sys.executable, str(ROOT_DIR / "tooling" / "build" / "build_rootfs.py"),
         "--arch", TARGET_ARCH, "--size", "1024"],
        cwd=ROOT_DIR,
        check=True,
    )


def push_rootfs_image(force: bool = False, allow_build: bool = True) -> None:
    """Deploy the canonical ext4 artifact selected for the connected ABI."""
    ensure_rootfs_image(allow_build=allow_build)
    rootfs_hash = sha256_file(ROOTFS_IMAGE)
    remote_hash = adb_shell(f"cat {DEVICE_ROOTFS_HASH}", as_root=True).stdout.strip()
    remote_image = adb_shell(f"test -f {DEVICE_ROOTFS_IMAGE}; echo $?", as_root=True).stdout.strip()
    if not force and remote_hash == rootfs_hash and remote_image.endswith("0"):
        log(f"Canonical {TARGET_ARCH} rootfs already matches the connected target.")
        return

    if not unmount_rootfs():
        raise RuntimeError("Cannot replace the rootfs image while it is still mounted.")

    if ANDROID_ZSTD_BINARY is None:
        upload_path = f"{DEVICE_ROOTFS_IMAGE}.upload"
        image_mb = ROOTFS_IMAGE.stat().st_size / (1024 * 1024)
        log(f"Pushing canonical {TARGET_ARCH} rootfs directly ({image_mb:.1f} MB)...")
        adb_shell(f"rm -f {upload_path}", as_root=True)
        run_adb("push", str(ROOTFS_IMAGE), upload_path)
        install = adb_shell(
            f"mv {upload_path} {DEVICE_ROOTFS_IMAGE} && "
            f"chmod 0600 {DEVICE_ROOTFS_IMAGE} && "
            f"echo {rootfs_hash} > {DEVICE_ROOTFS_HASH}",
            as_root=True,
        )
        if install.returncode != 0:
            raise RuntimeError(
                "Rootfs installation failed: " +
                (install.stderr.strip() or install.stdout.strip())
            )
        log(f"Canonical rootfs deployed: {DEVICE_ROOTFS_IMAGE}")
        return

    zstd = shutil.which("zstd")
    if not zstd:
        raise RuntimeError("Host zstd executable is required to deploy the rootfs image.")
    if not ANDROID_ZSTD_BINARY.is_file():
        raise RuntimeError(
            f"{TARGET_ARCH} Android zstd helper is missing: "
            f"{ANDROID_ZSTD_BINARY}. Run './main.py android' once without --no-build."
        )
    if (not ROOTFS_ARCHIVE.is_file() or
            ROOTFS_ARCHIVE.stat().st_mtime < ROOTFS_IMAGE.stat().st_mtime):
        log(f"Compressing canonical {TARGET_ARCH} rootfs with zstd...")
        subprocess.run(
            [zstd, "-T0", "-3", "-f", str(ROOTFS_IMAGE), "-o", str(ROOTFS_ARCHIVE)],
            check=True,
        )

    helper_mb = ANDROID_ZSTD_BINARY.stat().st_size / (1024 * 1024)
    log(f"Pushing {TARGET_ARCH} zstd helper ({helper_mb:.1f} MB) to {DEVICE_ZSTD_BINARY}...")
    run_adb("push", str(ANDROID_ZSTD_BINARY), DEVICE_ZSTD_BINARY)
    adb_shell(f"chmod 0755 {DEVICE_ZSTD_BINARY}", as_root=True)
    helper_probe = adb_shell(f"{DEVICE_ZSTD_BINARY} --version", as_root=True)
    if helper_probe.returncode != 0:
        raise RuntimeError(
            f"{TARGET_ARCH} zstd helper failed on the connected device: "
            + (helper_probe.stderr.strip() or helper_probe.stdout.strip())
        )

    archive_mb = ROOTFS_ARCHIVE.stat().st_size / (1024 * 1024)
    log(f"Pushing compressed rootfs ({archive_mb:.1f} MB)...")
    run_adb("push", str(ROOTFS_ARCHIVE), DEVICE_ROOTFS_ARCHIVE)
    log("Decompressing rootfs on the connected target...")
    result = adb_shell(
        f"{DEVICE_ZSTD_BINARY} -d -f {DEVICE_ROOTFS_ARCHIVE} -o {DEVICE_ROOTFS_IMAGE} && "
        f"chmod 0600 {DEVICE_ROOTFS_IMAGE} && rm -f {DEVICE_ROOTFS_ARCHIVE}",
        as_root=True,
    )
    if result.returncode != 0:
        raise RuntimeError("Rootfs decompression failed: " + result.stderr.strip())
    adb_shell(f"echo {rootfs_hash} > {DEVICE_ROOTFS_HASH}", as_root=True)
    log(f"Canonical rootfs deployed: {DEVICE_ROOTFS_IMAGE}")


def push_native_clients() -> None:
    """Stage Android-bionic graphics clients without mutating the rootfs image."""
    if not NATIVE_CLIENT_WRAPPER.is_file():
        raise RuntimeError(f"Native-client wrapper missing: {NATIVE_CLIENT_WRAPPER}")
    missing = [
        name for name, relative_path in NATIVE_CLIENT_ARTIFACTS.items()
        if not (BUILD_ANDROID_DIR / relative_path).is_file()
    ]
    if missing:
        raise RuntimeError(
            "Android-native client target(s) missing: " + ", ".join(missing) +
            f". Build them in {BUILD_ANDROID_DIR.name} first."
        )
    adb_shell(f"mkdir -p {DEVICE_NATIVE_CLIENT_DIR}", as_root=False)
    for name, relative_path in NATIVE_CLIENT_ARTIFACTS.items():
        run_adb("push", str(BUILD_ANDROID_DIR / relative_path),
                f"{DEVICE_NATIVE_CLIENT_DIR}/{name}")
    run_adb("push", str(NATIVE_CLIENT_WRAPPER), DEVICE_NATIVE_CLIENT_WRAPPER)
    native_paths = " ".join(
        f"{DEVICE_NATIVE_CLIENT_DIR}/{name}"
        for name in NATIVE_CLIENT_BINARIES
    )
    adb_shell(
        f"chmod 0755 {native_paths} {DEVICE_NATIVE_CLIENT_WRAPPER}",
        as_root=True,
    )
    log("Android-native AHardwareBuffer clients staged.")


def remove_native_clients() -> None:
    """Remove only the explicitly staged Android-native client payload."""
    for name in (*NATIVE_CLIENT_BINARIES, "launch"):
        adb_shell(f"rm -f {DEVICE_NATIVE_CLIENT_DIR}/{name}", as_root=True)
    adb_shell(f"rmdir {DEVICE_NATIVE_CLIENT_DIR}", as_root=True)


def mount_rootfs(native_clients: bool = False) -> None:
    """Loop-mount rootfs and bind Android kernel/runtime views into it."""
    # A prior process can die after creating only part of the bind graph. Never
    # layer a new launch over that stale graph: native-client file binds in
    # particular cannot be refreshed reliably when their old source was removed.
    if not unmount_rootfs():
        raise RuntimeError("Cannot mount rootfs over stale mount layers.")
    loop_device = attach_rootfs_loop()
    result = adb_shell(
        f"mkdir -p {DEVICE_ROOTFS_MOUNT} && "
        f"mount -t ext4 -o rw,noatime {shlex.quote(loop_device)} "
        f"{DEVICE_ROOTFS_MOUNT}",
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
            f"mount | grep -q \" {destination} \" || "
            f"mount {'--rbind' if source == '/dev' else '--bind'} "
            f"{source} {destination}",
            as_root=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"Rootfs bind mount failed ({source} -> {destination}): " +
                result.stderr.strip()
            )
        make_mount_rslave(destination)

    if native_clients:
        android_views = (
            ("/system", "system"),
            ("/vendor", "vendor"),
            ("/apex", "apex"),
            ("/system_ext", "system_ext"),
            ("/linkerconfig", "linkerconfig"),
        )
        for source, relative in android_views:
            source_exists = adb_shell(f"test -e {source}", as_root=True)
            if source_exists.returncode != 0:
                continue
            destination = f"{DEVICE_ROOTFS_MOUNT}/{relative}"
            adb_shell(f"mkdir -p {destination}", as_root=True)
            result = adb_shell(
                f"mount | grep -q \" {destination} \" || "
                f"mount --rbind {source} {destination}",
                as_root=True,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"Android runtime bind failed ({source} -> {destination}): " +
                    result.stderr.strip()
                )
            make_mount_rslave(destination)

        # Some Android mount implementations accept --rbind but only expose the
        # /apex tmpfs in the chroot, omitting the independently mounted APEX
        # packages below it. Bind every currently mounted package explicitly so
        # /system/bin/linker64 can resolve its /apex/com.android.runtime target.
        apex_mounts: list[str] = []
        mounts = adb_shell("cat /proc/mounts", as_root=True)
        for line in mounts.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[1].startswith("/apex/"):
                apex_mounts.append(fields[1].replace("\\040", " "))
        for source in sorted(set(apex_mounts), key=lambda path: (path.count("/"), path)):
            destination = f"{DEVICE_ROOTFS_MOUNT}{source}"
            adb_shell(f"mkdir -p {shlex.quote(destination)}", as_root=True)
            result = adb_shell(
                f"mount | grep -q \" {destination} \" || "
                f"mount --bind {shlex.quote(source)} {shlex.quote(destination)}",
                as_root=True,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"Android APEX bind failed ({source} -> {destination}): " +
                    result.stderr.strip()
                )
            make_mount_rslave(destination)

        native_root = f"{DEVICE_ROOTFS_MOUNT}/AndroidClients"
        adb_shell(f"mkdir -p {native_root}", as_root=True)
        result = adb_shell(
            f"mount --bind {DEVICE_NATIVE_CLIENT_DIR} {native_root}",
            as_root=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                "Android native-client directory bind failed: " +
                (result.stderr.strip() or result.stdout.strip())
            )
        make_mount_rslave(native_root)
        wrapper_targets = (
            f"{DEVICE_ROOTFS_MOUNT}/System/Core/lcl-desktop-shell",
            f"{DEVICE_ROOTFS_MOUNT}/System/Core/lcl-mobile-shell",
            f"{DEVICE_ROOTFS_MOUNT}/System/Core/lcl-js",
            f"{DEVICE_ROOTFS_MOUNT}/System/Applications/Terminal.app/Executables/Terminal",
            f"{DEVICE_ROOTFS_MOUNT}/System/Applications/UIDemo.app/Executables/UIDemo",
        )
        for target in wrapper_targets:
            result = adb_shell(
                f"mount --bind {DEVICE_NATIVE_CLIENT_WRAPPER} {target}",
                as_root=True,
            )
            if result.returncode != 0:
                detail = result.stderr.strip() or result.stdout.strip()
                raise RuntimeError(
                    f"Android native-client wrapper bind failed ({target}): {detail}"
                )
            make_mount_rslave(target)

        native_probe = adb_shell(
            f"ANDROID_DATA=/data LD_LIBRARY_PATH={ANDROID_NATIVE_LD_LIBRARY_PATH} "
            f"chroot {DEVICE_ROOTFS_MOUNT} /system/bin/linker64 --list "
            f"/AndroidClients/lcl-desktop-shell >/dev/null",
            as_root=True,
        )
        if native_probe.returncode != 0:
            raise RuntimeError(
                "Android native-client runtime probe failed: " +
                (native_probe.stderr.strip() or native_probe.stdout.strip())
            )
        log("Rootfs graphics clients routed to the Android AHardwareBuffer backend.")

    probe = adb_shell(
        f"chroot {DEVICE_ROOTFS_MOUNT} /System/Tools/bash -c \""
        f"test -S /Runtime/lcl-compositor.sock -o -d /Runtime; "
        f"echo ROOTFS_ARCH=\\$(/System/Tools/uname -m)\"",
        as_root=True,
    )
    if probe.returncode != 0 or f"ROOTFS_ARCH={TARGET_ARCH}" not in probe.stdout:
        raise RuntimeError(f"{TARGET_ARCH} rootfs chroot probe failed: " +
                           (probe.stderr.strip() or probe.stdout.strip()))
    log(f"Canonical {TARGET_ARCH} rootfs mounted and chroot probe passed.")


def push_rootfs_session_launcher() -> None:
    if not ROOTFS_SESSION_LAUNCHER.is_file():
        raise RuntimeError(f"Rootfs session launcher missing: {ROOTFS_SESSION_LAUNCHER}")
    run_adb("push", str(ROOTFS_SESSION_LAUNCHER), DEVICE_SESSION_LAUNCHER)
    adb_shell(f"chmod 0755 {DEVICE_SESSION_LAUNCHER}", as_root=True)


def prepare_runtime_for_launch() -> None:
    """Stop an existing LCL session, then clear its private runtime sockets."""
    process_names = (
        "lcl-core-android", "lcl-sessiond", "lcl-desktop-shell",
        "lcl-mobile-shell", "lcl-terminal",
    )

    def active_processes() -> list[str]:
        active: list[str] = []
        for name in process_names:
            result = adb_shell(f"pidof {name}", as_root=True)
            pids = [value for value in result.stdout.split() if value.isdigit()]
            if pids:
                active.append(f"{name} ({', '.join(pids)})")
        return active

    active = active_processes()
    restarted = bool(active)
    if restarted:
        log("Existing LCL session detected; restarting it...")
        log("  " + ", ".join(active))
        stop_rootfs_session()
        stop_lcl_process()
        unmount_rootfs()
        remove_native_clients()

        remaining = active_processes()
        if remaining:
            raise RuntimeError(
                "Could not stop the existing LCL session: " + ", ".join(remaining)
            )

    result = adb_shell(
        f"rm -f {DEVICE_COMPOSITOR_SOCKET} {DEVICE_SESSION_SOCKET}",
        as_root=True,
    )
    if result.returncode != 0:
        raise RuntimeError("Could not remove stale LCL runtime sockets: " + result.stderr.strip())
    if restarted:
        log("Previous LCL session stopped; runtime is ready for relaunch.")
    else:
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
    """Push compositor, raster service and optional HIDL bridge to device."""
    binary = BUILD_ANDROID_DIR / BINARY_NAME
    if not binary.is_file():
        err(f"Binary not found: {binary}")
        err(f"  -> Build first: cmake --build {BUILD_ANDROID_DIR} --target lcl-core-android")
        sys.exit(1)

    size_mb = binary.stat().st_size / (1024 * 1024)
    log(f"Pushing {BINARY_NAME} ({size_mb:.1f} MB) to {DEVICE_BINARY_PATH}...")

    run_adb("push", str(binary), DEVICE_TMP_DIR)
    adb_shell(f"chmod 755 {DEVICE_BINARY_PATH}", as_root=True)
    rasterd = BUILD_ANDROID_DIR / RASTERD_NAME
    if not rasterd.is_file():
        err(f"Raster service not found: {rasterd}")
        err(f"  -> Build target: lcl-rasterd-android")
        sys.exit(1)
    run_adb("push", str(rasterd), DEVICE_TMP_DIR)
    adb_shell(f"chmod 755 {DEVICE_RASTERD_PATH}", as_root=True)
    bridge = BUILD_ANDROID_DIR / HIDL_BRIDGE_NAME
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


def push_gestalt(gestalt_path: Path) -> None:
    """Push an explicit development Gestalt into the private runtime directory."""
    resolved = gestalt_path.expanduser().resolve()
    if not resolved.is_file():
        err(f"Gestalt file not found: {resolved}")
        sys.exit(1)
    log(f"Pushing Gestalt: {resolved}")
    # `adb push` runs as Android's shell user, which cannot traverse the
    # root-owned 0700 runtime directory. Stage in /data/local/tmp, then install
    # atomically as root without weakening runtime-directory permissions.
    adb_shell(f"rm -f {DEVICE_GESTALT_UPLOAD_PATH}", as_root=True)
    run_adb("push", str(resolved), DEVICE_GESTALT_UPLOAD_PATH)
    install = adb_shell(
        f"mv {DEVICE_GESTALT_UPLOAD_PATH} {DEVICE_GESTALT_PATH} && "
        f"chown root:root {DEVICE_GESTALT_PATH} && chmod 600 {DEVICE_GESTALT_PATH}",
        as_root=True,
    )
    if install.returncode != 0:
        err(f"Failed to install Gestalt at {DEVICE_GESTALT_PATH}: {install.stderr.strip()}")
        sys.exit(1)


def push_runtime_fonts() -> None:
    """Install the compositor's packaged fonts outside the rootfs chroot."""
    missing = [str(path) for path in RUNTIME_FONTS.values() if not path.is_file()]
    if missing:
        raise RuntimeError("Packaged runtime fonts are missing: " + ", ".join(missing))

    adb_shell(f"mkdir -p {DEVICE_FONT_DIR} && chmod 0755 {DEVICE_FONT_DIR}", as_root=True)
    for name, source in RUNTIME_FONTS.items():
        upload_path = f"{DEVICE_TMP_DIR}/lcl-{name}.upload"
        adb_shell(f"rm -f {shlex.quote(upload_path)}", as_root=True)
        run_adb("push", str(source), upload_path)
        result = adb_shell(
            f"mv {shlex.quote(upload_path)} {shlex.quote(f'{DEVICE_FONT_DIR}/{name}')} && "
            f"chown root:root {shlex.quote(f'{DEVICE_FONT_DIR}/{name}')} && "
            f"chmod 0644 {shlex.quote(f'{DEVICE_FONT_DIR}/{name}')}",
            as_root=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"Failed to install runtime font {name}: "
                + (result.stderr.strip() or result.stdout.strip())
            )
    log("Packaged compositor fonts installed in the Android runtime.")


def launch_lcl(no_stop_sysui: bool = False, logcat: bool = False,
               use_rootfs: bool = False, force_rootfs: bool = False,
               native_clients: bool = True,
               no_build: bool = False,
               gestalt_path: Path | None = None) -> None:
    """Main deployment logic: push binary, stop SysUI, launch LCL compositor."""

    log("=" * 60)
    log("  LCL Core Linux -- Android ADB Deployment")
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
    configure_target_for_abi(info["abi"])
    log(f"Selected {TARGET_ARCH} ADB deployment artifacts from {BUILD_ANDROID_DIR}.")
    if TARGET_ARCH == "x86_64" and native_clients:
        # The existing x86_64 Android build contains the compositor; canonical
        # glibc clients come from the x86_64 rootfs and use the SHM path.
        native_clients = False
        log("Using canonical x86_64 rootfs clients (Android-native client bundle is not required).")

    selected_gestalt_path = gestalt_path
    if selected_gestalt_path is None:
        selected_gestalt_path = device_gestalt_for_model(info["model"])
        if selected_gestalt_path is not None:
            log(f"Auto-selected Gestalt for ADB model '{info['model']}': {selected_gestalt_path}")
        else:
            log(
                f"No Gestalt profile for ADB model '{info['model']}' in "
                f"{DEVICE_GESTALT_DIR}; using the platform default."
            )

    # 4. Reject an overlapping session before changing deployed artifacts.
    runtime_dir = setup_runtime_dir()
    prepare_runtime_for_launch()
    if selected_gestalt_path is not None:
        push_gestalt(selected_gestalt_path)
    else:
        # The shell launcher reads the same runtime profile through the rootfs
        # bind mount. Do not let a previous device/session select a stale shell.
        adb_shell(f"rm -f {DEVICE_GESTALT_PATH}", as_root=True)

    # 4b. Push binary
    push_binary()
    push_runtime_fonts()

    # 4c. Deploy and mount the canonical ABI-matched glibc userspace when requested.
    if use_rootfs:
        try:
            push_rootfs_image(force=force_rootfs, allow_build=not no_build)
            if native_clients:
                push_native_clients()
            mount_rootfs(native_clients=native_clients)
            push_rootfs_session_launcher()
        except Exception:
            # These steps run before the main session try/finally. Do not leave
            # loop or bind mounts behind when rootfs preparation fails early.
            unmount_rootfs()
            if native_clients:
                remove_native_clients()
            raise

    # 5. Stop System UI for display takeover (unless suppressed)
    if not no_stop_sysui:
        stop_system_ui()
        time.sleep(1)
    else:
        log("Skipping System UI stop (--no-stop-sysui mode).")

    # 6. Launch lcl-core-android as root
    log("Launching lcl-core-android compositor on device...")
    log("  The LCL desktop should appear on the connected target.")
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
            f"LCL_RUNTIME_DIR={runtime_dir} LCL_FONT_ROOT={DEVICE_FONT_DIR} "
            f"LCL_RASTERD_PATH={DEVICE_RASTERD_PATH} ANDROID_DATA=/data "
            f"LD_LIBRARY_PATH={DEVICE_TMP_DIR}:/system/lib64:/vendor/lib64:/system_ext/lib64"
        )
        if selected_gestalt_path is not None:
            launch_env += f" LCL_GESTALT_PATH={DEVICE_GESTALT_PATH}"
        for var in ("LCL_DEBUG_OVERLAY", "LCL_DEBUG", "LCL_LOG_LEVEL", "LCL_SHOW_FPS"):
            val = os.environ.get(var)
            if val:
                launch_env += f" {var}={val}"
        lcl_proc = subprocess.Popen(adb_shell_command(
            f"{launch_env} {DEVICE_BINARY_PATH} 2>&1 | tee {DEVICE_LOG_PATH}",
            as_root=True,
        ))

        if use_rootfs:
            log("Waiting for Android compositor IPC before starting canonical userspace...")
            if not wait_for_compositor_socket(process=lcl_proc):
                raise RuntimeError(
                    "LCL compositor did not expose a live listening IPC socket "
                    "before the rootfs session timeout."
                )
            session_proc = subprocess.Popen(adb_shell_command(
                f"{DEVICE_SESSION_LAUNCHER} 2>&1",
                as_root=True,
            ))
            log(f"Canonical {TARGET_ARCH} rootfs session launched.")

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
            if native_clients:
                remove_native_clients()

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
    if not check_root():
        raise RuntimeError("Cannot restore Android UI without root access.")
    configure_target_for_abi(get_device_info()["abi"])
    stop_rootfs_session()
    stop_lcl_process()
    unmount_rootfs()
    remove_native_clients()
    restore_system_ui()
    log("Done.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="LCL OS - Android Device/Emulator Deployment (ADB + Root)"
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
        help="Mount and launch the canonical glibc rootfs matching the target ABI"
    )
    parser.add_argument(
        "--push-rootfs",
        action="store_true",
        help="Force re-upload of the canonical ABI-matched rootfs (implies --rootfs)"
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Require existing artifacts matching the connected target ABI"
    )
    parser.add_argument(
        "--software-clients",
        action="store_true",
        help="Keep canonical glibc SHM clients instead of Android AHardwareBuffer clients"
    )
    parser.add_argument(
        "--gestalt",
        type=Path,
        metavar="JSON",
        help="Override automatic config/devices/<ADB model>.json selection"
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
        native_clients=not args.software_clients,
        no_build=args.no_build,
        gestalt_path=args.gestalt,
    )


if __name__ == "__main__":
    main()
