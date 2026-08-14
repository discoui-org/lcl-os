#!/usr/bin/env python3
"""
LCL OS - Limine Bootable ISO Builder (Pure Python)
Builds bootable UEFI/BIOS hybrid ISO images for x86_64 and aarch64.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_ROOT / "build"
ISO_ROOT = BUILD_DIR / "iso_root"


def log(msg: str) -> None:
    print(f"[LCL ISO] {msg}")


def err(msg: str) -> None:
    print(f"[LCL ISO ERROR] {msg}", file=sys.stderr)


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


def find_limine_datadir() -> Path | None:
    # 1. Try limine CLI
    limine_bin = shutil.which("limine")
    if limine_bin:
        try:
            out = subprocess.check_output([limine_bin, "--print-datadir"], text=True).strip()
            if out and Path(out).is_dir():
                return Path(out)
        except Exception:
            pass

    # 2. Known directories
    candidates = [
        Path("/usr/share/limine"),
        Path("/usr/local/share/limine"),
        Path("/opt/homebrew/share/limine"),
    ]
    for c in candidates:
        if c.is_dir():
            return c
    return None


def build_iso(arch: str = "x86_64") -> Path:
    target_arch = normalize_arch(arch)
    output_iso = BUILD_DIR / f"lcl-os-{target_arch}.iso"
    generic_iso = BUILD_DIR / "lcl-os.iso"

    print("====================================================")
    print(f"  LCL OS - Limine Bootable ISO ({target_arch}) Builder")
    print("====================================================")

    # 1. Package kernel & initramfs via run_qemu.py
    log(f"Compiling binaries & packaging kernel/initramfs for {target_arch} via Docker...")
    run_qemu = SCRIPT_DIR / "run_qemu.py"
    subprocess.check_call([sys.executable, str(run_qemu), "--package-only", "--arch", target_arch])

    kernel_src = BUILD_DIR / "qemu-cache" / "vmlinuz"
    initramfs_src = BUILD_DIR / "initramfs.cpio.gz"

    if not kernel_src.is_file() or not initramfs_src.is_file():
        err(f"Missing kernel ({kernel_src}) or initramfs ({initramfs_src})!")
        sys.exit(1)

    # 2. Find Limine datadir
    limine_datadir = find_limine_datadir()
    if not limine_datadir:
        err("Limine datadir not found. Please install limine (e.g. pacman -S limine / apt install limine).")
        sys.exit(1)
    log(f"Using Limine datadir: {limine_datadir}")

    # 3. Stage ISO Root Directory
    log(f"Staging filesystem at {ISO_ROOT}...")
    if ISO_ROOT.exists():
        shutil.rmtree(ISO_ROOT)
    (ISO_ROOT / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
    (ISO_ROOT / "boot").mkdir(parents=True, exist_ok=True)

    if target_arch == "aarch64":
        bootaa64 = limine_datadir / "BOOTAA64.EFI"
        uefi_cd = limine_datadir / "limine-uefi-cd.bin"
        if bootaa64.is_file():
            shutil.copy2(bootaa64, ISO_ROOT / "EFI" / "BOOT" / "BOOTAA64.EFI")
        if uefi_cd.is_file():
            shutil.copy2(uefi_cd, ISO_ROOT / "boot" / "limine-uefi-cd.bin")
    else:
        bootx64 = limine_datadir / "BOOTX64.EFI"
        bios_cd = limine_datadir / "limine-bios-cd.bin"
        bios_sys = limine_datadir / "limine-bios.sys"
        uefi_cd = limine_datadir / "limine-uefi-cd.bin"

        if bootx64.is_file():
            shutil.copy2(bootx64, ISO_ROOT / "EFI" / "BOOT" / "BOOTX64.EFI")
        if bios_cd.is_file():
            shutil.copy2(bios_cd, ISO_ROOT / "boot" / "limine-bios-cd.bin")
        if bios_sys.is_file():
            shutil.copy2(bios_sys, ISO_ROOT / "boot" / "limine-bios.sys")
            shutil.copy2(bios_sys, ISO_ROOT / "boot" / "limine.sys")
            shutil.copy2(bios_sys, ISO_ROOT / "limine-bios.sys")
            shutil.copy2(bios_sys, ISO_ROOT / "limine.sys")
        if uefi_cd.is_file():
            shutil.copy2(uefi_cd, ISO_ROOT / "boot" / "limine-uefi-cd.bin")

    # Copy limine.conf
    limine_conf_src = PROJECT_ROOT / "iso_root" / "boot" / "limine.conf"
    if limine_conf_src.is_file():
        shutil.copy2(limine_conf_src, ISO_ROOT / "boot" / "limine.conf")
    else:
        default_conf = (
            "timeout: 3\n\n"
            "/LCL OS\n"
            "    protocol: linux\n"
            "    kernel_path: boot():/boot/vmlinuz\n"
            "    initrd_path: boot():/boot/initramfs.cpio.gz\n"
            "    cmdline: console=tty0 lcl.scale=1\n"
            "    resolution: preferred\n"
        )
        (ISO_ROOT / "boot" / "limine.conf").write_text(default_conf, encoding="utf-8")
    shutil.copy2(ISO_ROOT / "boot" / "limine.conf", ISO_ROOT / "limine.conf")

    # Copy kernel & initramfs
    log("Copying kernel and initramfs into ISO root...")
    shutil.copy2(kernel_src, ISO_ROOT / "boot" / "vmlinuz")
    shutil.copy2(initramfs_src, ISO_ROOT / "boot" / "initramfs.cpio.gz")

    # 4. Generate ISO with xorriso
    xorriso = shutil.which("xorriso")
    if not xorriso:
        err("xorriso command not found. Please install xorriso.")
        sys.exit(1)

    log("Generating ISO image with xorriso...")
    bios_cd_path = ISO_ROOT / "boot" / "limine-bios-cd.bin"
    if target_arch == "x86_64" and bios_cd_path.is_file():
        cmd = [
            xorriso,
            "-as", "mkisofs",
            "-b", "boot/limine-bios-cd.bin",
            "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
            "--efi-boot", "boot/limine-uefi-cd.bin",
            "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
            str(ISO_ROOT),
            "-o", str(output_iso),
        ]
        subprocess.check_call(cmd)
        limine_bin = shutil.which("limine")
        if limine_bin:
            try:
                subprocess.run([limine_bin, "bios-install", str(output_iso)], check=False)
            except Exception:
                pass
    else:
        # UEFI-only / aarch64
        cmd = [
            xorriso,
            "-as", "mkisofs",
            "-r", "-J",
            "--efi-boot", "boot/limine-uefi-cd.bin",
            "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
            str(ISO_ROOT),
            "-o", str(output_iso),
        ]
        try:
            subprocess.check_call(cmd)
        except subprocess.CalledProcessError:
            subprocess.check_call([xorriso, "-as", "mkisofs", "-r", "-J", str(ISO_ROOT), "-o", str(output_iso)])

    # Copy generic alias
    shutil.copy2(output_iso, generic_iso)

    size_mb = output_iso.stat().st_size / (1024 * 1024)
    print("====================================================")
    print(f"  ISO Build Complete!")
    print(f"  Artifact: {output_iso} ({size_mb:.1f} MB)")
    print("====================================================")
    return output_iso


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL OS Limine ISO builder")
    parser.add_argument(
        "--arch", "-a",
        metavar="ARCH",
        help="Target architecture: x86_64 or aarch64",
    )
    args = parser.parse_args()
    build_iso(arch=args.arch or os.environ.get("ARCH"))


if __name__ == "__main__":
    main()
