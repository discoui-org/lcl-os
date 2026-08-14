#!/usr/bin/env python3
"""
LCL OS - UTM & utmctl Manager (Pure Python)
Builds bootable ISO, generates/configures LCL-OS.utm bundle, and launches via utmctl on macOS.
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
    # 1. System PATH
    p = shutil.which("utmctl")
    if p:
        return Path(p)

    # 2. UTM.app bundle locations
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


def generate_utm_bundle(iso_file: Path, arch: str = "aarch64") -> Path:
    log(f"Creating/updating UTM bundle at {UTM_BUNDLE}...")
    UTM_BUNDLE.mkdir(parents=True, exist_ok=True)
    data_dir = UTM_BUNDLE / "Data"
    data_dir.mkdir(parents=True, exist_ok=True)

    # Copy ISO to bundle data directory as lcl-os.iso
    dest_iso = data_dir / "lcl-os.iso"
    if not dest_iso.is_file() or dest_iso.stat().st_mtime < iso_file.stat().st_mtime:
        log(f"Copying {iso_file.name} to {dest_iso}...")
        shutil.copy2(iso_file, dest_iso)

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
        "Drive": [
            {
                "Interface": "virtio",
                "Removable": True,
                "ReadOnly": True,
                "ImageName": "lcl-os.iso",
                "Identifier": str(uuid.uuid5(uuid.NAMESPACE_DNS, "lcl-os-iso-drive")),
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

    log("UTM bundle configuration generated successfully with VirtIO GPU GL (Metal).")
    return UTM_BUNDLE


def launch_utm(arch: str = "aarch64") -> None:
    target_arch = normalize_arch(arch)
    iso_file = BUILD_DIR / f"lcl-os-{target_arch}.iso"

    # 1. Build ISO if needed
    if not iso_file.is_file():
        log(f"ISO {iso_file} not found. Generating bootable ISO first...")
        build_iso_py = SCRIPT_DIR / "build_iso.py"
        subprocess.check_call([sys.executable, str(build_iso_py), "--arch", target_arch])

    # 2. Prepare UTM bundle
    bundle = generate_utm_bundle(iso_file, arch=target_arch)

    # 3. Launch via utmctl or open
    utmctl = find_utmctl()
    if utmctl:
        log(f"Found utmctl at {utmctl}")
        try:
            # Check if LCL-OS is already registered in utmctl
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
        log("VM opened in UTM with Metal GPU hardware acceleration!")
    else:
        err(f"UTM launch is supported on macOS. Bundle created at: {bundle}")


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL OS UTM launcher")
    parser.add_argument(
        "--arch", "-a",
        metavar="ARCH",
        help="Target architecture: aarch64 (ARM64) or x86_64",
    )
    args = parser.parse_args()
    launch_utm(arch=args.arch or os.environ.get("ARCH"))


if __name__ == "__main__":
    main()
