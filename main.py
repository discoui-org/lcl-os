#!/usr/bin/env python3
"""
LCL Core Linux - Unified CLI Management & Launcher (main.py)
Provides a clean, cross-platform CLI for building, running QEMU, ISO generation, flashing, and testing.

Usage:
  ./main.py qemu [--native] [--gpu] [--arch aarch64|x86_64]
  ./main.py avd [--avd-name lcl-phone] [--no-window] [--rebuild]
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

    subprocess.check_call(qemu_args)


def cmd_utm(args: argparse.Namespace) -> None:
    run_utm_py = SCRIPTS_DIR / "run_utm.py"
    arch = normalize_arch(args.arch)
    subprocess.check_call([sys.executable, str(run_utm_py), "--arch", arch])


def cmd_avd(args: argparse.Namespace) -> None:
    run_avd_py = SCRIPTS_DIR / "run_avd.py"
    avd_args = [sys.executable, str(run_avd_py)]
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
    p_qemu.add_argument("--scale", type=float, metavar="FACTOR", help="UI scale factor (e.g. 1.5, 2.0)")
    p_qemu.add_argument("--width", type=int, metavar="PX", help="Display width in pixels")
    p_qemu.add_argument("--height", type=int, metavar="PX", help="Display height in pixels")
    p_qemu.add_argument("--iso", action="store_true", help="Boot from CD-ROM ISO image")
    p_qemu.add_argument("--uefi", action="store_true", help="Boot with UEFI firmware")
    p_qemu.add_argument("--usb", metavar="VENDOR:PRODUCT", help="Pass through host USB device")
    p_qemu.add_argument("--trace-frames", action="store_true", help="Enable layout/render trace")
    p_qemu.add_argument("--debug-layout", action="store_true", help="Draw widget bounds overlay")
    p_qemu.add_argument("--debug-overlay", action="store_true", help="Draw compositor FPS overlay")
    p_qemu.add_argument("--no-build", action="store_true", help="Skip Docker build/package; launch QEMU with existing cached artifacts")

    # ---- utm ----
    p_utm = subparsers.add_parser("utm", help="Build ISO and launch via UTM / utmctl (Metal 3D)")
    p_utm.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (aarch64 or x86_64)")

    # ---- avd ----
    p_avd = subparsers.add_parser("avd", help="Build & launch LCL OS on Android AVD emulator")
    p_avd.add_argument("--avd-name", metavar="NAME", default="lcl-phone", help="Target AVD name (default: lcl-phone)")
    p_avd.add_argument("--show-kernel", action="store_true", help="Display live guest kernel and init boot logs in terminal")
    p_avd.add_argument("--no-window", action="store_true", help="Run emulator headless without GUI window")
    p_avd.add_argument("--no-build", action="store_true", help="Skip artifact build & packaging")
    p_avd.add_argument("--rebuild", action="store_true", help="Force clean rebuild of all targets")

    # ---- build ----
    p_build = subparsers.add_parser("build", help="Build LCL OS binaries via Docker")
    p_build.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- package ----
    p_pkg = subparsers.add_parser("package", help="Build binaries and package initramfs")
    p_pkg.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")

    # ---- rootfs ----
    p_rootfs = subparsers.add_parser("rootfs", help="Build single canonical ext4 rootfs image")
    p_rootfs.add_argument("--arch", "-a", metavar="ARCH", help="Target architecture (x86_64 or aarch64)")
    p_rootfs.add_argument("--size", "-s", type=int, default=512, metavar="MB", help="Filesystem size in MB (default: 512)")

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

    dispatch = {
        "qemu": cmd_qemu,
        "utm": cmd_utm,
        "avd": cmd_avd,
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
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
