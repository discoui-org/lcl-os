#!/usr/bin/env python3
"""
LCL OS - UTM & utmctl Manager (Pure Python)
Packages kernel & initramfs, configures LCL-OS.utm bundle with Direct Kernel Boot
and Metal-accelerated VirtIO GPU GL, and launches via utmctl/UTM on macOS.
"""

from __future__ import annotations

import argparse
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
BUILD_DIR = ROOT_DIR / "build"
UTM_BUNDLE = BUILD_DIR / "LCL-OS.utm"


def log(msg: str) -> None:
    print(f"[LCL UTM] {msg}")


def err(msg: str) -> None:
    print(f"[LCL UTM ERROR] {msg}", file=sys.stderr)


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


def find_utmctl() -> Path | None:
    p = shutil.which("utmctl")
    if p:
        return Path(p)

    candidates = [
        Path("/Applications/UTM.app/Contents/MacOS/utmctl"),
        Path.home() / "Applications/UTM.app/Contents/MacOS/utmctl",
        Path("/usr/local/bin/utmctl"),
        Path("/opt/homebrew/bin/utmctl"),
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def generate_utm_bundle(kernel_src: Path, initramfs_src: Path, arch: str = "aarch64") -> Path:
    log(f"Creating/updating UTM bundle at {UTM_BUNDLE} (Direct Kernel Boot)...")
    UTM_BUNDLE.mkdir(parents=True, exist_ok=True)
    data_dir = UTM_BUNDLE / "Data"
    data_dir.mkdir(parents=True, exist_ok=True)

    dest_kernel = data_dir / "vmlinuz"
    dest_initrd = data_dir / "initramfs.cpio.gz"

    if not dest_kernel.is_file() or dest_kernel.stat().st_mtime < kernel_src.stat().st_mtime:
        shutil.copy2(kernel_src, dest_kernel)
    if not dest_initrd.is_file() or dest_initrd.stat().st_mtime < initramfs_src.stat().st_mtime:
        shutil.copy2(initramfs_src, dest_initrd)

    serial_console = (
        "console=ttyAMA0,115200 console=tty0 earlycon"
        if arch == "aarch64"
        else "console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200"
    )
    boot_args = (
        f"{serial_console} "
        f"video=2560x1600-32@60 lcl.scale=2.0 lcl.width=2560 lcl.height=1600 "
        f"rdinit=/init loglevel=6"
    )

    # Create config.plist for UTM QEMU with VirtIO GPU GL (Metal 3D Acceleration)
    config: dict = {
        "ConfigurationVersion": 4,
        "Information": {
            "Name": "LCL-OS",
            "Notes": "LCL Core Linux Desktop OS (Metal GPU Hardware Accelerated)",
            "Icon": "linux",
            "UUID": str(uuid.uuid5(uuid.NAMESPACE_DNS, "lcl-os.org")),
        },
        "System": {
            "Architecture": arch,
            "Target": "virt" if arch == "aarch64" else "q35",
            "MemorySize": 2048,
            "CPUCount": 2,
            "CPUFlagsAdd": [],
            "CPUFlagsRemove": [],
            "JITCacheSize": 0,
            "ForceMulticore": True,
        },
        "QEMU": {
            "Hypervisor": True,
            "PS2Controller": False,
            "RNGDevice": True,
            "BalloonDevice": True,
            "DirectKernelPath": "vmlinuz",
            "DirectInitrdPath": "initramfs.cpio.gz",
            "DirectBootArgs": boot_args,
        },
        "Display": [
            {
                "Hardware": "virtio-gpu-gl-pci" if arch == "aarch64" else "virtio-vga-gl",
                "NativeResolution": True,
                "DynamicResolution": True,
                "UpscalingFilter": "Linear",
                "DownscalingFilter": "Linear",
            }
        ],
        "Input": {
            "Keyboard": "usb",
            "Pointer": "usb-tablet",
        },
        "Sound": [
            {
                "Hardware": "intel-hda",
            }
        ],
    }

    config_plist = UTM_BUNDLE / "config.plist"
    with config_plist.open("wb") as f:
        plistlib.dump(config, f)

    log("UTM bundle configured successfully (Direct Kernel Boot + Metal GPU).")
    return UTM_BUNDLE


def launch_utm(arch: str = "aarch64", rebuild: bool = False, no_build: bool = False) -> None:
    target_arch = normalize_arch(arch)

    # 1. Package kernel & initramfs via run_qemu.py --package-only
    kernel_src = BUILD_DIR / "qemu-cache" / "vmlinuz"
    initramfs_src = BUILD_DIR / "initramfs.cpio.gz"

    if rebuild or (not no_build and (not kernel_src.is_file() or not initramfs_src.is_file())):
        log(f"Packaging kernel & initramfs for {target_arch}...")
        run_qemu_py = SCRIPT_DIR / "run_qemu.py"
        pkg_args = [sys.executable, str(run_qemu_py), "--package-only", "--arch", target_arch]
        if rebuild:
            pkg_args.append("--rebuild")
        subprocess.check_call(pkg_args)
    elif not kernel_src.is_file() or not initramfs_src.is_file():
        err("Missing kernel or initramfs artifacts for --no-build.")
        sys.exit(1)

    # 2. Prepare UTM bundle
    bundle = generate_utm_bundle(kernel_src, initramfs_src, arch=target_arch)

    # 3. Launch via utmctl or open
    utmctl = find_utmctl()
    if utmctl:
        log(f"Found utmctl at {utmctl}")
        try:
            out = subprocess.check_output([str(utmctl), "list"], text=True)
            if "LCL-OS" in out:
                log("Starting LCL-OS VM via utmctl...")
                subprocess.run([str(utmctl), "start", "LCL-OS"], check=False)
                return
        except Exception as exc:
            log(f"utmctl query: {exc}")

    # Fallback to macOS open command for .utm bundle
    if platform.system().lower() == "darwin":
        log("Opening LCL-OS.utm bundle in UTM...")
        subprocess.check_call(["open", str(bundle)])
        log("VM launched in UTM with Metal GPU hardware acceleration!")
    else:
        err(f"UTM launch is supported on macOS. Bundle created at: {bundle}")


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL OS UTM launcher")
    parser.add_argument(
        "--arch", "-a",
        metavar="ARCH",
        help="Target architecture: aarch64 (ARM64) or x86_64",
    )
    parser.add_argument("--no-build", action="store_true", help="Skip build step; launch existing VM directly")
    parser.add_argument("--rebuild", action="store_true", help="Force clean rebuild of kernel and initramfs")
    args = parser.parse_args()

    if getattr(args, "rebuild", False) and getattr(args, "no_build", False):
        parser.error("Cannot specify both --rebuild and --no-build.")

    launch_utm(
        arch=args.arch or os.environ.get("ARCH"),
        rebuild=args.rebuild,
        no_build=args.no_build,
    )


if __name__ == "__main__":
    main()
