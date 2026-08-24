#!/usr/bin/env python3
"""
LCL Core Linux - Unified CLI Management & Launcher (main.py)
Provides a clean, cross-platform CLI for building, running QEMU, ISO generation, flashing, and testing.

Usage:
  ./main.py qemu [--native] [--gpu] [--arch aarch64|x86_64]
  ./main.py qemu --mobile [--skin pixel_8_pro] [--gestalt profile.json]
  ./main.py avd [--avd-name lcl-phone] [--no-window] [--rebuild]
  ./main.py android [--no-build] [--rebuild] [--push-rootfs]
  ./main.py build [--arch ...]
  ./main.py iso [--arch ...]
  ./main.py flash [--dev /dev/sdX]
  ./main.py fonts
  ./main.py test
  ./main.py clean
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = ROOT_DIR / "scripts"
BUILD_DIR = ROOT_DIR / "build"
ANDROID_BUILD_DIR = ROOT_DIR / "build-android-arm64"
ANDROID_HIDL_ROOT = BUILD_DIR / "android-hidl-v31"
ANDROID_ROOTFS_IMAGE = BUILD_DIR / "rootfs" / "lcl-rootfs-aarch64.ext4"
ANDROID_ZSTD_BINARY = BUILD_DIR / "android-tools-arm64" / "zstd"


def log(msg: str) -> None:
    print(f"[LCL] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL ERROR] {msg}", file=sys.stderr, flush=True)


def normalize_arch(arch_str: str | None) -> str:
    if not arch_str:
        host_m = platform.machine().lower()
        if host_m in ("aarch64", "arm64", "armv8", "armv9"):
            return "aarch64"
        return "x86_64"
    a = arch_str.lower().strip()
    if a in ("aarch64", "arm64", "arm"):
        return "aarch64"
    return "x86_64"


def cmd_qemu(args: argparse.Namespace) -> None:
    if getattr(args, "mobile", False):
        if args.utm:
            err("--mobile uses the LCL Device Viewer and cannot be combined with --utm.")
            sys.exit(2)
        if args.iso or args.uefi or args.usb:
            err("--mobile Device Viewer does not support --iso, --uefi, or --usb.")
            sys.exit(2)
        if normalize_arch(args.arch) != "x86_64":
            err("--mobile Device Viewer currently supports only x86_64.")
            sys.exit(2)

        viewer_args = [sys.executable, str(ROOT_DIR / "emulator" / "main.py")]
        if args.skin:
            viewer_args.extend(["--skin", args.skin])
        if args.gestalt:
            viewer_args.extend(["--gestalt", str(args.gestalt)])
        if not args.no_build:
            viewer_args.append("--build")
        if args.rebuild:
            viewer_args.append("--rebuild")
        subprocess.check_call(viewer_args, cwd=ROOT_DIR)
        return

    if getattr(args, "utm", False) or (platform.system().lower() == "darwin" and (Path("/Applications/UTM.app").is_dir() or (Path.home() / "Applications/UTM.app").is_dir())):
        cmd_utm(args)
        return

    run_qemu_py = SCRIPTS_DIR / "run_qemu.py"
    qemu_args = [sys.executable, str(run_qemu_py), "--run"]

    arch = normalize_arch(args.arch)
    qemu_args.extend(["--arch", arch])

    if args.native:
        qemu_args.append("--native")
    if args.gpu:
        qemu_args.append("--gpu")
    if args.retina:
        qemu_args.append("--retina")
    if getattr(args, "mobile", False):
        qemu_args.append("--mobile")
    if args.iso:
        qemu_args.append("--iso")
    if args.uefi:
        qemu_args.append("--uefi")
    if args.scale is not None:
        qemu_args.extend(["--scale", str(args.scale)])
    if args.width is not None:
        qemu_args.extend(["--width", str(args.width)])
    if args.height is not None:
        qemu_args.extend(["--height", str(args.height)])
    if args.gestalt:
        qemu_args.extend(["--gestalt", str(args.gestalt)])
    if args.usb:
        qemu_args.extend(["--usb", str(args.usb)])
    if args.trace_frames:
        qemu_args.append("--trace-frames")
    if args.debug_layout:
        qemu_args.append("--debug-layout")
    if args.debug_overlay:
        qemu_args.append("--debug-overlay")
    if args.no_build:
        qemu_args.append("--no-build")
    if getattr(args, "rebuild", False):
        qemu_args.append("--rebuild")

    subprocess.check_call(qemu_args)


def cmd_utm(args: argparse.Namespace) -> None:
    run_utm_py = SCRIPTS_DIR / "run_utm.py"
    arch = normalize_arch(args.arch)
    utm_args = [sys.executable, str(run_utm_py), "--arch", arch]
    if getattr(args, "rebuild", False):
        utm_args.append("--rebuild")
    if getattr(args, "no_build", False):
        utm_args.append("--no-build")
    subprocess.check_call(utm_args)


def cmd_avd(args: argparse.Namespace) -> None:
    arch = normalize_arch(getattr(args, "arch", None))
    if arch != "x86_64":
        err(f"AVD platform substrate only supports 'x86_64' (requested: '{arch}').")
        sys.exit(1)

    run_avd_py = SCRIPTS_DIR / "run_avd.py"
    avd_args = [sys.executable, str(run_avd_py), "--arch", arch]
    if getattr(args, "avd_name", None):
        avd_args.extend(["--avd-name", str(args.avd_name)])
    if getattr(args, "show_kernel", False):
        avd_args.append("--show-kernel")
    if getattr(args, "no_window", False):
        avd_args.append("--no-window")
    if getattr(args, "no_build", False):
        avd_args.append("--no-build")
    if getattr(args, "rebuild", False):
        avd_args.append("--rebuild")
    subprocess.check_call(avd_args)


def find_android_ndk() -> Path:
    """Resolve an installed Android NDK without assuming one SDK layout."""
    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        value = os.environ.get(variable)
        if value:
            ndk = Path(value).expanduser()
            if (ndk / "build/cmake/android.toolchain.cmake").is_file():
                return ndk.resolve()

    cache = ANDROID_BUILD_DIR / "CMakeCache.txt"
    if cache.is_file():
        prefix = "CMAKE_TOOLCHAIN_FILE:FILEPATH="
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(prefix):
                toolchain = Path(line.removeprefix(prefix))
                if toolchain.is_file():
                    return toolchain.parent.parent.parent.resolve()

    sdk_roots: list[Path] = []
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(variable)
        if value:
            sdk_roots.append(Path(value).expanduser())
    sdk_roots.append(Path.home() / "Android/Sdk")

    candidates: list[Path] = []
    for sdk_root in sdk_roots:
        ndk_root = sdk_root / "ndk"
        if ndk_root.is_dir():
            candidates.extend(path for path in ndk_root.iterdir() if path.is_dir())
        ndk_bundle = sdk_root / "ndk-bundle"
        if ndk_bundle.is_dir():
            candidates.append(ndk_bundle)

    for ndk in sorted(candidates, reverse=True):
        if (ndk / "build/cmake/android.toolchain.cmake").is_file():
            return ndk.resolve()

    raise RuntimeError(
        "Android NDK not found. Set ANDROID_NDK_HOME or install the NDK under "
        "$ANDROID_HOME/ndk/."
    )


def build_android_phone_artifacts(args: argparse.Namespace, use_rootfs: bool) -> None:
    """Build the Android substrate and optional canonical ARM64 userspace."""
    hidl_config = ANDROID_HIDL_ROOT / "hidl-config.cmake"
    if not hidl_config.is_file():
        log("Preparing official Android HIDL headers and device libraries...")
        subprocess.check_call(
            [sys.executable, str(SCRIPTS_DIR / "prepare_android_hidl.py")],
            cwd=ROOT_DIR,
        )

    ndk = find_android_ndk()
    if use_rootfs and (args.rebuild or not ANDROID_ZSTD_BINARY.is_file()):
        log("Building pinned ARM64 zstd deployment helper...")
        subprocess.check_call(
            [
                sys.executable, str(SCRIPTS_DIR / "prepare_android_zstd.py"),
                "--ndk", str(ndk), "--output", str(ANDROID_ZSTD_BINARY),
            ],
            cwd=ROOT_DIR,
        )
    log(f"Configuring ARM64 Android compositor with NDK: {ndk}")
    configure_args = [
        "cmake", "-S", str(ROOT_DIR), "-B", str(ANDROID_BUILD_DIR),
        f"-DCMAKE_TOOLCHAIN_FILE={ndk / 'build/cmake/android.toolchain.cmake'}",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-33",
        f"-DLCL_ANDROID_HIDL_ROOT={ANDROID_HIDL_ROOT}",
    ]
    subprocess.check_call(configure_args, cwd=ROOT_DIR)

    jobs = max(1, int(args.jobs))
    android_targets = ["lcl-core-android"]
    if use_rootfs and not args.software_clients:
        android_targets.extend(("lcl-desktop-shell", "lcl-terminal"))
    build_args = [
        "cmake", "--build", str(ANDROID_BUILD_DIR),
        "--target", *android_targets, "-j", str(jobs),
    ]
    if args.rebuild:
        build_args.append("--clean-first")
    log("Building ARM64 Android compositor (AIDL + HIDL)...")
    subprocess.check_call(build_args, cwd=ROOT_DIR)

    if use_rootfs:
        log("Building canonical ARM64 rootfs...")
        subprocess.check_call(
            [
                sys.executable, str(SCRIPTS_DIR / "build_rootfs.py"),
                "--arch", "aarch64", "--size", str(args.size),
            ],
            cwd=ROOT_DIR,
        )


def require_android_phone_artifacts(use_rootfs: bool, native_clients: bool) -> None:
    """Make --no-build strict instead of silently building missing files."""
    required = [ANDROID_BUILD_DIR / "lcl-core-android"]
    if use_rootfs:
        required.extend((ANDROID_ROOTFS_IMAGE, ANDROID_ZSTD_BINARY))
    if use_rootfs and native_clients:
        required.extend((
            ANDROID_BUILD_DIR / "lcl-desktop-shell",
            ANDROID_BUILD_DIR / "lcl-terminal",
        ))
    missing = [path for path in required if not path.is_file()]
    if missing:
        formatted = "\n".join(f"  - {path}" for path in missing)
        raise RuntimeError(
            "--no-build was requested, but required Android artifacts are missing:\n"
            f"{formatted}\nRun './main.py android' once to build them."
        )


def cmd_android(args: argparse.Namespace) -> None:
    """Build and launch LCL OS with the connected device's matching Gestalt."""
    deploy_args = [sys.executable, str(SCRIPTS_DIR / "deploy_android_device.py")]
    if args.restore_only:
        run_android_deploy([*deploy_args, "--restore-only"])
        return

    use_rootfs = not args.compositor_only
    native_clients = use_rootfs and not args.software_clients
    if args.no_build:
        require_android_phone_artifacts(use_rootfs, native_clients)
        log("Skipping Android compositor and rootfs build (--no-build).")
    else:
        build_android_phone_artifacts(args, use_rootfs)

    if use_rootfs:
        deploy_args.append("--push-rootfs" if args.push_rootfs else "--rootfs")
    if args.no_stop_sysui:
        deploy_args.append("--no-stop-sysui")
    if args.logcat:
        deploy_args.append("--logcat")
    if args.software_clients:
        deploy_args.append("--software-clients")
    if args.gestalt:
        deploy_args.extend(("--gestalt", str(args.gestalt)))

    run_android_deploy(deploy_args)


def run_android_deploy(command: list[str]) -> None:
    """Let the deployment child finish device cleanup after terminal SIGINT."""
    process = subprocess.Popen(command, cwd=ROOT_DIR)
    interrupted = False
    while True:
        try:
            return_code = process.wait()
            break
        except KeyboardInterrupt:
            interrupted = True
            log("Waiting for Android deployment cleanup to finish...")
    if return_code != 0:
        if interrupted and return_code == 130:
            return
        raise subprocess.CalledProcessError(return_code, command)


def cmd_build(args: argparse.Namespace) -> None:
    run_qemu_py = SCRIPTS_DIR / "run_qemu.py"
    arch = normalize_arch(args.arch)
    subprocess.check_call([sys.executable, str(run_qemu_py), "--build-only", "--arch", arch])


def cmd_package(args: argparse.Namespace) -> None:
    run_qemu_py = SCRIPTS_DIR / "run_qemu.py"
    arch = normalize_arch(args.arch)
    subprocess.check_call([sys.executable, str(run_qemu_py), "--package-only", "--arch", arch])


def cmd_iso(args: argparse.Namespace) -> None:
    build_iso_py = SCRIPTS_DIR / "build_iso.py"
    arch = normalize_arch(args.arch)
    subprocess.check_call([sys.executable, str(build_iso_py), "--arch", arch])


def cmd_rootfs(args: argparse.Namespace) -> None:
    build_rootfs_py = SCRIPTS_DIR / "build_rootfs.py"
    arch = normalize_arch(args.arch)
    rootfs_args = [sys.executable, str(build_rootfs_py), "--arch", arch]
    if getattr(args, "size", None):
        rootfs_args.extend(["--size", str(args.size)])
    subprocess.check_call(rootfs_args)


def cmd_flash(args: argparse.Namespace) -> None:
    arch = normalize_arch(args.arch)
    iso_file = BUILD_DIR / f"lcl-os-{arch}.iso"
    if not iso_file.is_file():
        log(f"ISO {iso_file} not found. Building ISO first...")
        build_iso_py = SCRIPTS_DIR / "build_iso.py"
        subprocess.check_call([sys.executable, str(build_iso_py), "--arch", arch])

    dev = args.dev or os.environ.get("USB_DEV") or "/dev/disk/by-id/usb-SanDisk_Cruzer_Blade_04019222101620123055-0:0"
    if not Path(dev).exists():
        err(f"Target USB device '{dev}' not found.")
        err("Specify device with: ./main.py flash --dev /dev/sdX")
        sys.exit(1)

    log(f"Flashing {iso_file} to {dev}...")
    pv = shutil.which("pv")
    if pv:
        cmd = f"pv '{iso_file}' | sudo dd of='{dev}' bs=4M conv=fsync status=none"
        subprocess.check_call(cmd, shell=True)
    else:
        subprocess.check_call(["sudo", "dd", f"if={iso_file}", f"of={dev}", "bs=4M", "status=progress", "conv=fsync"])
    subprocess.run(["sync"], check=False)
    log(f"Successfully flashed {iso_file} to {dev}!")


def cmd_fonts(args: argparse.Namespace) -> None:
    fetch_fonts_py = SCRIPTS_DIR / "fetch_fonts.py"
    subprocess.check_call([sys.executable, str(fetch_fonts_py)])


def cmd_clean(args: argparse.Namespace) -> None:
    run_qemu_py = SCRIPTS_DIR / "run_qemu.py"
    subprocess.check_call([sys.executable, str(run_qemu_py), "--clean"])


def cmd_test(args: argparse.Namespace) -> None:
    build_host = ROOT_DIR / "build_host"
    if not build_host.is_dir():
        log("Configuring host build directory for tests...")
        subprocess.check_call(["cmake", "-B", str(build_host), "-S", str(ROOT_DIR), "-DCMAKE_BUILD_TYPE=Debug"])
    log("Compiling test suite...")
    subprocess.check_call(["cmake", "--build", str(build_host), "-j", str(os.cpu_count() or 4)])
    log("Running ctest...")
    subprocess.check_call(["ctest", "--test-dir", str(build_host), "--output-on-failure"])


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="main.py",
        description="LCL Core Linux - Unified System Management CLI",
    )
    subparsers = parser.add_subparsers(dest="command", help="Available subcommands")

    # ---- qemu ----
    p_qemu = subparsers.add_parser("qemu", help="Build & launch QEMU virtual machine")
    p_qemu.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")
    p_qemu.add_argument("--utm", action="store_true", help="Launch via UTM / utmctl on macOS")
    p_qemu.add_argument("--native", "-n", action="store_true", help="Match host resolution + fullscreen")
    p_qemu.add_argument("--gpu", "-g", action="store_true", help="Enable 3D VirGL GPU acceleration")
    p_qemu.add_argument("--retina", action="store_true", help="13\" MacBook Air Retina (2560x1600 @ 2.0x)")
    p_qemu.add_argument("--mobile", action="store_true", help="Portrait mobile display (1179x2556 @ 2.0x)")
    p_qemu.add_argument("--skin", metavar="PIXEL_SKIN", help="Pixel skin for --mobile Device Viewer (e.g. pixel_8_pro)")
    p_qemu.add_argument("--gestalt", type=Path, metavar="JSON", help="Use an explicit Gestalt JSON in the guest")
    p_qemu.add_argument("--scale", type=float, metavar="FACTOR", help="UI scale factor (e.g. 1.5, 2.0)")
    p_qemu.add_argument("--width", type=int, metavar="PX", help="Display width in pixels")
    p_qemu.add_argument("--height", type=int, metavar="PX", help="Display height in pixels")
    p_qemu.add_argument("--iso", action="store_true", help="Boot from CD-ROM ISO image")
    p_qemu.add_argument("--uefi", action="store_true", help="Boot with UEFI firmware")
    p_qemu.add_argument("--usb", metavar="VENDOR:PRODUCT", help="Pass through host USB device")
    p_qemu.add_argument("--trace-frames", action="store_true", help="Enable layout/render trace")
    p_qemu.add_argument("--debug-layout", action="store_true", help="Draw widget bounds overlay")
    p_qemu.add_argument("--debug-overlay", action="store_true", help="Draw compositor FPS overlay")
    p_qemu.add_argument("--no-build", action="store_true", help="Skip build/package; launch QEMU with existing cached artifacts")
    p_qemu.add_argument("--rebuild", action="store_true", help="Force clean rebuild of all binaries and canonical rootfs image")

    # ---- utm ----
    p_utm = subparsers.add_parser("utm", help="Build ISO and launch via UTM / utmctl (Metal 3D)")
    p_utm.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (aarch64 or x86_64)")
    p_utm.add_argument("--no-build", action="store_true", help="Skip build; launch existing UTM VM directly")
    p_utm.add_argument("--rebuild", action="store_true", help="Force clean rebuild of ISO and VM image")

    # ---- avd ----
    p_avd = subparsers.add_parser("avd", help="Build & launch LCL OS on Android AVD emulator")
    p_avd.add_argument("--arch", "-a", metavar="ARCH", default="x86_64", help="Target architecture (only x86_64 supported on AVD)")
    p_avd.add_argument("--avd-name", metavar="NAME", default="lcl-phone", help="Target AVD name (default: lcl-phone)")
    p_avd.add_argument("--show-kernel", action="store_true", help="Display live guest kernel and init boot logs in terminal")
    p_avd.add_argument("--no-window", action="store_true", help="Run emulator headless without GUI window")
    p_avd.add_argument("--no-build", action="store_true", help="Skip artifact build & packaging")
    p_avd.add_argument("--rebuild", action="store_true", help="Force clean rebuild of all targets")

    # ---- android ----
    p_android = subparsers.add_parser(
        "android",
        help="Build & launch LCL OS on a connected rooted ARM64 Android phone",
    )
    p_android.add_argument(
        "--no-build", action="store_true",
        help="Skip all builds and require existing Android/rootfs artifacts",
    )
    p_android.add_argument(
        "--rebuild", action="store_true",
        help="Clean-rebuild the Android compositor and regenerate the rootfs",
    )
    p_android.add_argument(
        "--jobs", "-j", type=int, default=2, metavar="N",
        help="Parallel jobs for the Android compositor build (default: 2)",
    )
    p_android.add_argument(
        "--size", "-s", type=int, default=1024, metavar="MB",
        help="Canonical ARM64 rootfs size in MB (default: 1024)",
    )
    p_android.add_argument(
        "--push-rootfs", action="store_true",
        help="Force re-upload of the canonical rootfs image",
    )
    p_android.add_argument(
        "--compositor-only", action="store_true",
        help="Launch only the Android compositor without the canonical rootfs session",
    )
    p_android.add_argument(
        "--no-stop-sysui", action="store_true",
        help="Keep Android System UI running (overlay diagnostics only)",
    )
    p_android.add_argument(
        "--logcat", action="store_true",
        help="Show Android logcat alongside LCL compositor output",
    )
    p_android.add_argument(
        "--restore-only", action="store_true",
        help="Stop LCL and restore Android UI without building or launching",
    )
    p_android.add_argument(
        "--software-clients", action="store_true",
        help="Use canonical glibc SHM clients instead of Android AHardwareBuffer clients",
    )
    p_android.add_argument(
        "--gestalt", type=Path, metavar="JSON",
        help="Override automatic devices/<ADB model>.json selection",
    )

    # ---- build ----
    p_build = subparsers.add_parser("build", help="Build LCL OS binaries via Docker")
    p_build.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- package ----
    p_pkg = subparsers.add_parser("package", help="Build binaries and package initramfs")
    p_pkg.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- rootfs ----
    p_rootfs = subparsers.add_parser("rootfs", help="Build single canonical ext4 rootfs image")
    p_rootfs.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")
    p_rootfs.add_argument("--size", "-s", type=int, default=1024, metavar="MB", help="Filesystem size in MB (default: 1024)")

    # ---- iso ----
    p_iso = subparsers.add_parser("iso", help="Build Limine bootable ISO image")
    p_iso.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- flash ----
    p_flash = subparsers.add_parser("flash", help="Flash ISO directly to a USB drive")
    p_flash.add_argument("--dev", "-d", metavar="PATH", help="Target USB device path (e.g. /dev/sdX)")
    p_flash.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- fonts ----
    subparsers.add_parser("fonts", help="Download font assets (Inter, JetBrains Mono, Liberation)")

    # ---- test ----
    subparsers.add_parser("test", help="Compile and run unit test suite")

    # ---- clean ----
    subparsers.add_parser("clean", help="Clean build artifacts")

    # Default to 'qemu' when no subcommand is given
    if len(sys.argv) == 1:
        parser.print_help()
        return

    args = parser.parse_args()

    if getattr(args, "rebuild", False) and getattr(args, "no_build", False):
        parser.error("Cannot specify both --rebuild and --no-build.")
    if args.command == "android":
        if args.jobs < 1:
            parser.error("android --jobs must be at least 1.")
        if args.size < 128:
            parser.error("android --size must be at least 128 MB.")
        if args.push_rootfs and args.compositor_only:
            parser.error("android --push-rootfs cannot be combined with --compositor-only.")

    dispatch = {
        "qemu": cmd_qemu,
        "utm": cmd_utm,
        "avd": cmd_avd,
        "android": cmd_android,
        "build": cmd_build,
        "package": cmd_package,
        "rootfs": cmd_rootfs,
        "iso": cmd_iso,
        "flash": cmd_flash,
        "fonts": cmd_fonts,
        "test": cmd_test,
        "clean": cmd_clean,
    }

    handler = dispatch.get(args.command)
    if handler:
        handler(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode or 1)
    except RuntimeError as exc:
        err(str(exc))
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
