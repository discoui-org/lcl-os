#!/usr/bin/env python3
"""LCL QEMU launcher — Linux native; macOS/Windows via Docker."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
BUILD_DIR = ROOT_DIR / "build"
BINARY = BUILD_DIR / "lcl-core"
OPEN_BIN = BUILD_DIR / "lcl-open"
INITRAMFS_DIR = BUILD_DIR / "initramfs_root"
INITRAMFS_IMG = BUILD_DIR / "initramfs.cpio.gz"
CACHE_DIR = BUILD_DIR / "qemu-cache"
KERNEL_CACHE = CACHE_DIR / "vmlinuz"
DOCKER_IMAGE = "lcl-os-qemu-builder:latest"
DOCKERFILE = SCRIPT_DIR / "Dockerfile.qemu"


def log(msg: str) -> None:
    print(f"[LCL QEMU] {msg}")


def err(msg: str) -> None:
    print(f"[LCL QEMU ERROR] {msg}", file=sys.stderr)


def host_os() -> str:
    return platform.system().lower()  # linux, darwin, windows


def is_linux() -> bool:
    return host_os() == "linux"


def which(name: str) -> str | None:
    return shutil.which(name)


def run(cmd: list[str] | str, **kwargs) -> subprocess.CompletedProcess:
    if isinstance(cmd, str):
        return subprocess.run(cmd, shell=True, check=True, **kwargs)
    return subprocess.run(cmd, check=True, **kwargs)


def ensure_dirs() -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    CACHE_DIR.mkdir(parents=True, exist_ok=True)


def find_qemu() -> str:
    qemu = which("qemu-system-x86_64")
    if not qemu:
        err("qemu-system-x86_64 is not installed.")
        if host_os() == "darwin":
            err("Install with: brew install qemu")
        elif host_os() == "windows":
            err("Install QEMU and ensure qemu-system-x86_64 is on PATH.")
        else:
            err("Install qemu-system-x86_64 via your package manager.")
        sys.exit(1)
    return qemu


def ensure_fonts() -> None:
    inter = ROOT_DIR / "assets" / "fonts" / "inter"
    if not inter.is_dir() or not any(inter.iterdir()):
        fetch = SCRIPT_DIR / "fetch_fonts.sh"
        if fetch.is_file():
            log("Font assets missing. Executing fetch_fonts.sh...")
            run(["bash", str(fetch)])


def cmake_build() -> None:
    log("Building lcl-core binary...")
    run(
        [
            "cmake",
            "-B",
            str(BUILD_DIR),
            "-S",
            str(ROOT_DIR),
            "-DCMAKE_BUILD_TYPE=Debug",
        ]
    )
    run(["cmake", "--build", str(BUILD_DIR)])


def locate_kernel() -> Path:
    """Find a Linux kernel image. Prefer real /boot packages over uname -r
    (uname inside Docker often reports the host/VM kernel, not the image)."""
    if KERNEL_CACHE.is_file() and not os.environ.get("LCL_IGNORE_KERNEL_CACHE"):
        return KERNEL_CACHE

    uname_r = platform.release()
    candidates = [
        Path("/boot/vmlinuz"),
        Path(f"/boot/vmlinuz-{uname_r}"),
        Path("/boot/vmlinuz-linux"),
        Path(f"/lib/modules/{uname_r}/vmlinuz"),
        Path("/lib/modules/7.1.4-1-cachyos/vmlinuz"),
    ]
    # Packaged kernels in the build image (Docker)
    boot = Path("/boot")
    if boot.is_dir():
        candidates.extend(sorted(boot.glob("vmlinuz-*"), reverse=True))

    for path in candidates:
        if path.is_file() or path.is_symlink():
            resolved = path.resolve()
            if resolved.is_file():
                return resolved

    for base in (Path("/lib/modules"), Path("/boot")):
        if not base.exists():
            continue
        found = sorted(base.rglob("vmlinuz*"))
        for path in found:
            resolved = path.resolve()
            if resolved.is_file():
                return resolved

    err("No Linux kernel image found.")
    sys.exit(1)


def locate_module_tree(kernel_path: Path) -> tuple[Path, str]:
    """Return (modules_dir, kver) matching the kernel we will boot."""
    # vmlinuz-6.8.0-51-generic -> 6.8.0-51-generic
    name = kernel_path.name
    if name.startswith("vmlinuz-"):
        kver = name[len("vmlinuz-") :]
        mod = Path("/lib/modules") / kver
        if (mod / "kernel").is_dir() or mod.is_dir():
            return mod, kver

    parent = kernel_path.parent
    if (parent / "kernel").is_dir():
        return parent, parent.name

    # Fall back to any installed module tree (Docker image)
    modules_root = Path("/lib/modules")
    if modules_root.is_dir():
        for entry in sorted(modules_root.iterdir(), reverse=True):
            if (entry / "kernel").is_dir():
                return entry, entry.name

    uname_r = platform.release()
    return Path(f"/lib/modules/{uname_r}"), uname_r


def copy_ldd_deps(binary: Path, dest_lib: Path) -> None:
    if not binary.is_file():
        return
    try:
        out = subprocess.check_output(["ldd", str(binary)], text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return
    for line in out.splitlines():
        parts = line.strip().split()
        lib_path = None
        if "=>" in parts:
            idx = parts.index("=>")
            if idx + 1 < len(parts) and parts[idx + 1].startswith("/"):
                lib_path = parts[idx + 1]
        elif parts and parts[0].startswith("/"):
            lib_path = parts[0]
        if lib_path and Path(lib_path).is_file():
            shutil.copy2(lib_path, dest_lib / Path(lib_path).name, follow_symlinks=True)


def write_text(path: Path, content: str, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    if executable:
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def package_kernel_modules(kernel_path: Path, init_root: Path) -> None:
    kmod_base, kver = locate_module_tree(kernel_path)

    target = init_root / "usr" / "lib" / "modules" / kver
    if (target / "kernel").is_dir():
        log(f"Using cached kernel modules in {target}")
        return

    log(f"Packaging DRM & input kernel modules ({kver})...")
    target.mkdir(parents=True, exist_ok=True)
    for rel in (
        "kernel/drivers/gpu/drm",
        "kernel/drivers/virtio",
        "kernel/drivers/hid",
        "kernel/drivers/input",
    ):
        src = kmod_base / rel
        if src.is_dir():
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists():
                shutil.rmtree(dst)
            shutil.copytree(src, dst, symlinks=False)

    for zst in target.rglob("*.ko.zst"):
        try:
            run(["zstd", "-d", "--rm", str(zst)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            pass

    if which("depmod"):
        try:
            run(
                ["depmod", "-b", str(init_root), kver],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            pass


def package_initramfs(kernel_path: Path) -> None:
    log("Preparing initramfs root directory structure...")
    if INITRAMFS_DIR.exists():
        shutil.rmtree(INITRAMFS_DIR)

    for sub in (
        "proc",
        "sys",
        "dev",
        "tmp",
        "etc",
        "usr/bin",
        "usr/lib",
        "usr/share",
        "home/user/Desktop",
        "home/user/Documents",
        "home/user/Downloads",
        "home/user/Applications",
    ):
        (INITRAMFS_DIR / sub).mkdir(parents=True, exist_ok=True)

    for link, target in (
        ("bin", "usr/bin"),
        ("sbin", "usr/bin"),
        ("lib", "usr/lib"),
        ("lib64", "usr/lib"),
    ):
        link_path = INITRAMFS_DIR / link
        if not link_path.exists():
            link_path.symlink_to(target)

    if not BINARY.is_file():
        err(f"Binary not found: {BINARY}")
        sys.exit(1)

    shutil.copy2(BINARY, INITRAMFS_DIR / "usr" / "bin" / "lcl-core")

    host_bins = {
        "sh": Path("/bin/sh"),
        "bash": Path(which("bash") or "/usr/bin/bash"),
        "mount": Path("/bin/mount"),
        "mkdir": Path("/bin/mkdir"),
        "sleep": Path("/bin/sleep"),
        "ls": Path("/bin/ls"),
        "printf": Path("/usr/bin/printf"),
        "modprobe": Path("/sbin/modprobe"),
    }

    log("Packaging GNU Bash and essential utilities...")
    dest_bin = INITRAMFS_DIR / "usr" / "bin"
    dest_lib = INITRAMFS_DIR / "usr" / "lib"

    for name, src in host_bins.items():
        if src.is_file():
            shutil.copy2(src, dest_bin / name, follow_symlinks=True)

    libinput_share = Path("/usr/share/libinput")
    if libinput_share.is_dir():
        shutil.copytree(libinput_share, INITRAMFS_DIR / "usr" / "share" / "libinput", dirs_exist_ok=True)

    ensure_fonts()
    log("Packaging system fonts into /usr/share/fonts/...")
    fonts_src = ROOT_DIR / "assets" / "fonts"
    fonts_dst = INITRAMFS_DIR / "usr" / "share" / "fonts"
    fonts_dst.mkdir(parents=True, exist_ok=True)
    if fonts_src.is_dir():
        for item in fonts_src.iterdir():
            dest = fonts_dst / item.name
            if item.is_dir():
                if dest.exists():
                    shutil.rmtree(dest)
                shutil.copytree(item, dest)
            else:
                shutil.copy2(item, dest)

    write_text(
        dest_bin / "clear",
        "#!/bin/sh\nprintf \"\\033[2J\\033[H\"\n",
        executable=True,
    )

    write_text(
        INITRAMFS_DIR / "etc" / "profile",
        """export HOME=/home/user
export TERM=xterm-256color
export HISTSIZE=500
export HISTFILESIZE=1000
alias ls='ls --color=auto'
alias ll='ls -la'
if [ -f /home/user/.bashrc ]; then
    . /home/user/.bashrc
fi
""",
    )

    write_text(
        INITRAMFS_DIR / "home" / "user" / ".bashrc",
        """export TERM=xterm-256color
export PS1='\\[\\033[1;34m\\]\\W\\[\\033[0m\\] ❯ '
export HISTSIZE=500
alias ls='ls --color=auto'
alias ll='ls -la'
bind 'set completion-ignore-case on' 2>/dev/null || true
bind 'set show-all-if-ambiguous on' 2>/dev/null || true
bind 'TAB:menu-complete' 2>/dev/null || true
bind '"\\e[Z":menu-complete-backward' 2>/dev/null || true
""",
    )

    write_text(
        INITRAMFS_DIR / "home" / "user" / ".shrc",
        "export PS1='\\W ❯ '\nalias ls='ls --color=auto'\nalias ll='ls -la'\n",
    )
    write_text(INITRAMFS_DIR / "home" / "user" / ".profile", ". /home/user/.bashrc\n")

    if OPEN_BIN.is_file():
        shutil.copy2(OPEN_BIN, dest_bin / "open")
    else:
        write_text(
            dest_bin / "open",
            "#!/bin/sh\necho \"Usage: open <app_name.app | path_to_app>\"\n",
            executable=True,
        )

    term_app = INITRAMFS_DIR / "home" / "user" / "Applications" / "Terminal.app"
    (term_app / "bin").mkdir(parents=True, exist_ok=True)
    (term_app / "assets").mkdir(parents=True, exist_ok=True)
    write_text(
        term_app / "metadata.json",
        """{
    "name": "LCL Terminal",
    "executable": "bin/terminal",
    "version": "1.0.0",
    "icon": "assets/icon.png"
}
""",
    )
    write_text(
        term_app / "bin" / "terminal",
        """#!/bin/sh
echo "===================================================="
echo "          LCL OS Terminal Subsystem App             "
echo "===================================================="
echo "Interactive PTY Shell active on seat0."
echo "===================================================="
""",
        executable=True,
    )

    sysmon_app = INITRAMFS_DIR / "home" / "user" / "Applications" / "SystemMonitor.app"
    (sysmon_app / "bin").mkdir(parents=True, exist_ok=True)
    (sysmon_app / "assets").mkdir(parents=True, exist_ok=True)
    write_text(
        sysmon_app / "metadata.json",
        """{
    "name": "System Monitor",
    "executable": "bin/sysmon",
    "version": "1.0.0",
    "icon": "assets/icon.png"
}
""",
    )
    write_text(
        sysmon_app / "bin" / "sysmon",
        """#!/bin/sh
echo "===================================================="
echo "          LCL OS System Monitor v1.0.0              "
echo "===================================================="
echo "Kernel: $(uname -a)"
echo "Uptime: $(uptime 2>/dev/null || echo '0 mins')"
echo "Memory: 2048 MB RAM Allocated"
echo "===================================================="
""",
        executable=True,
    )

    log("Resolving dynamic library dependencies...")
    bins = [BINARY, OPEN_BIN] + [p for p in host_bins.values() if p.is_file()]
    for bin_path in bins:
        copy_ldd_deps(bin_path, dest_lib)

    package_kernel_modules(kernel_path, INITRAMFS_DIR)

    # Cache kernel next to artifacts when packaging inside Docker/Linux
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    resolved = kernel_path.resolve()
    if resolved != KERNEL_CACHE.resolve():
        shutil.copy2(resolved, KERNEL_CACHE, follow_symlinks=True)

    write_text(
        INITRAMFS_DIR / "init",
        """#!/bin/sh
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/dri /dev/input /home/user/Desktop /home/user/Documents /home/user/Downloads /home/user/Applications
mount -t devpts devpts /dev/pts 2>/dev/null || true

mknod -m 666 /dev/dri/card0 c 226 0 2>/dev/null || true
mknod -m 666 /dev/dri/renderD128 c 226 128 2>/dev/null || true
mknod -m 666 /dev/fb0 c 29 0 2>/dev/null || true

modprobe virtio_dma_buf 2>/dev/null || true
modprobe virtio_gpu 2>/dev/null || true
modprobe bochs 2>/dev/null || true
modprobe drm 2>/dev/null || true
modprobe drm_kms_helper 2>/dev/null || true
modprobe usbhid 2>/dev/null || true
modprobe hid_generic 2>/dev/null || true
modprobe evdev 2>/dev/null || true

sleep 0.5

echo "===================================================="
echo "  LCL Core Linux (LCL) - QEMU Direct Kernel Boot   "
echo "===================================================="
echo "DRM devices detected:"
ls -la /dev/dri/ 2>/dev/null || echo "  (none)"
echo "Input devices detected:"
ls /dev/input/ 2>/dev/null || echo "  (none yet)"
exec /bin/lcl-core
""",
        executable=True,
    )

    log("Packaging initramfs cpio archive...")
    compress = ["pigz", "-1"] if which("pigz") else ["gzip", "-1"]
    find_proc = subprocess.Popen(
        ["find", ".", "-print0"],
        cwd=str(INITRAMFS_DIR),
        stdout=subprocess.PIPE,
    )
    cpio_proc = subprocess.Popen(
        ["cpio", "--null", "-ov", "--format=newc"],
        cwd=str(INITRAMFS_DIR),
        stdin=find_proc.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if find_proc.stdout:
        find_proc.stdout.close()
    with open(INITRAMFS_IMG, "wb") as out_f:
        gz_proc = subprocess.Popen(compress, stdin=cpio_proc.stdout, stdout=out_f)
        if cpio_proc.stdout:
            cpio_proc.stdout.close()
        gz_proc.communicate()
        cpio_proc.wait()
        find_proc.wait()
        if gz_proc.returncode not in (0, None) and gz_proc.returncode != 0:
            err("Failed to compress initramfs")
            sys.exit(1)

    log(f"Initramfs image built at: {INITRAMFS_IMG}")


def docker_available() -> bool:
    if not which("docker"):
        return False
    try:
        subprocess.run(
            ["docker", "info"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def ensure_docker_image() -> None:
    log(f"Ensuring Docker image {DOCKER_IMAGE}...")
    inspect = subprocess.run(
        ["docker", "image", "inspect", DOCKER_IMAGE],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if inspect.returncode != 0:
        log("Building Docker builder image (first run may take a few minutes)...")
        run(
            [
                "docker",
                "build",
                "--platform",
                "linux/amd64",
                "-t",
                DOCKER_IMAGE,
                "-f",
                str(DOCKERFILE),
                str(SCRIPT_DIR),
            ]
        )


def docker_run(args: list[str]) -> None:
    ensure_docker_image()
    # Resolve Windows paths for Docker Desktop if needed
    mount = str(ROOT_DIR)
    cmd = [
        "docker",
        "run",
        "--rm",
        "--platform",
        "linux/amd64",
        "-v",
        f"{mount}:/src",
        "-w",
        "/src",
        DOCKER_IMAGE,
        "python3",
        "scripts/run_qemu.py",
        *args,
        "--inside-docker",
    ]
    log("Running build/package inside Docker (linux/amd64)...")
    run(cmd)


def native_build_and_package() -> Path:
    ensure_dirs()
    cmake_build()
    kernel = locate_kernel()
    log(f"Host Kernel: {kernel}")
    package_initramfs(kernel)
    return KERNEL_CACHE if KERNEL_CACHE.is_file() else kernel


def prepare_artifacts(force_docker: bool = False) -> Path:
    ensure_dirs()
    use_docker = force_docker or not is_linux()

    if use_docker:
        if not docker_available():
            err("Docker is required on macOS/Windows (and when --docker is set).")
            if host_os() == "darwin":
                err("Install Docker Desktop: https://www.docker.com/products/docker-desktop/")
            sys.exit(1)
        docker_run(["--package-only"])
        if not KERNEL_CACHE.is_file() or not INITRAMFS_IMG.is_file():
            err("Docker packaging did not produce kernel/initramfs artifacts.")
            sys.exit(1)
        return KERNEL_CACHE

    return native_build_and_package()


def build_only(force_docker: bool = False) -> None:
    ensure_dirs()
    if force_docker or not is_linux():
        if not docker_available():
            err("Docker is required to build on this platform.")
            sys.exit(1)
        docker_run(["--build-only"])
        log(f"Linux binaries in {BUILD_DIR} (produced via Docker)")
        return
    cmake_build()
    log(f"Build complete: {BINARY}")


def _clamp_hz(value: float | int | None) -> int | None:
    if value is None:
        return None
    try:
        hz = int(round(float(value)))
    except (TypeError, ValueError):
        return None
    if 30 <= hz <= 500:
        return hz
    return None


def _clamp_dim(value: float | int | None) -> int | None:
    if value is None:
        return None
    try:
        dim = int(round(float(value)))
    except (TypeError, ValueError):
        return None
    if 320 <= dim <= 7680:
        return dim
    return None


def _clamp_scale(value: float | int | None) -> float:
    if value is None:
        return 1.0
    try:
        scale = float(value)
    except (TypeError, ValueError):
        return 1.0
    if scale < 0.5:
        return 1.0
    if scale > 4.0:
        return 4.0
    # Snap common DPRs
    for candidate in (1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0):
        if abs(scale - candidate) < 0.08:
            return candidate
    return round(scale, 2)


@dataclass
class HostDisplay:
    """Host primary display geometry for QEMU + guest LCL."""

    width: int = 1280
    height: int = 800
    refresh_hz: int = 60
    scale: float = 1.0
    logical_width: int | None = None
    logical_height: int | None = None
    physical_width: int | None = None
    physical_height: int | None = None


def detect_host_display() -> HostDisplay:
    """Best-effort primary display: resolution, refresh, and DPI scale."""
    system = host_os()
    info = HostDisplay()

    if system == "darwin":
        try:
            out = subprocess.check_output(
                ["system_profiler", "SPDisplaysDataType", "-json"],
                text=True,
                stderr=subprocess.DEVNULL,
            )
            data = json.loads(out)
            main_disp = None
            first_disp = None
            for gpu in data.get("SPDisplaysDataType", []):
                for disp in gpu.get("spdisplays_ndrvs", []) or []:
                    if first_disp is None:
                        first_disp = disp
                    if disp.get("spdisplays_main") == "spdisplays_yes":
                        main_disp = disp
                        break
                if main_disp:
                    break
            disp = main_disp or first_disp
            if disp:
                # logical: "1710 x 1112 @ 60.00Hz"
                res = str(disp.get("_spdisplays_resolution", ""))
                m = re.search(
                    r"(\d+)\s*x\s*(\d+)(?:\s*@\s*([0-9.]+)\s*Hz)?",
                    res,
                    re.I,
                )
                if m:
                    info.logical_width = _clamp_dim(m.group(1))
                    info.logical_height = _clamp_dim(m.group(2))
                    hz = _clamp_hz(m.group(3)) if m.group(3) else None
                    if hz:
                        info.refresh_hz = hz
                pixels = str(disp.get("_spdisplays_pixels", ""))
                pm = re.search(r"(\d+)\s*x\s*(\d+)", pixels)
                if pm:
                    info.physical_width = _clamp_dim(pm.group(1))
                    info.physical_height = _clamp_dim(pm.group(2))
                if not info.refresh_hz or info.refresh_hz == 60:
                    blob = " ".join(str(v) for v in disp.values())
                    hm = re.search(r"@\s*([0-9.]+)\s*Hz", blob, re.I)
                    if hm:
                        hz = _clamp_hz(hm.group(1))
                        if hz:
                            info.refresh_hz = hz
        except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError, OSError):
            pass

    elif system == "linux":
        if which("xrandr"):
            try:
                out = subprocess.check_output(["xrandr"], text=True, stderr=subprocess.DEVNULL)
                # "eDP-1 connected primary 1920x1080+0+0 ..."
                for line in out.splitlines():
                    if " connected" not in line:
                        continue
                    m = re.search(r"\s(\d+)x(\d+)\+\d+\+\d+", line)
                    if m:
                        info.logical_width = _clamp_dim(m.group(1))
                        info.logical_height = _clamp_dim(m.group(2))
                        info.physical_width = info.logical_width
                        info.physical_height = info.logical_height
                    break
                for line in out.splitlines():
                    if "*" not in line:
                        continue
                    nums = re.findall(r"([0-9]+(?:\.[0-9]+)?)\s*\*", line)
                    if nums:
                        hz = _clamp_hz(nums[0])
                        if hz:
                            info.refresh_hz = hz
                        break
                    rm = re.search(r"\b(\d{2,3}(?:\.\d+)?)\b", line)
                    if rm:
                        hz = _clamp_hz(rm.group(1))
                        if hz:
                            info.refresh_hz = hz
                            break
            except (subprocess.CalledProcessError, FileNotFoundError, OSError):
                pass
        # Wayland / GNOME scale
        if which("gsettings"):
            try:
                out = subprocess.check_output(
                    ["gsettings", "get", "org.gnome.desktop.interface", "scaling-factor"],
                    text=True,
                    stderr=subprocess.DEVNULL,
                )
                sm = re.search(r"(\d+)", out)
                if sm and int(sm.group(1)) >= 1:
                    info.scale = _clamp_scale(sm.group(1))
            except (subprocess.CalledProcessError, FileNotFoundError, OSError):
                pass
        if info.logical_width is None:
            try:
                drm = Path("/sys/class/drm")
                if drm.is_dir():
                    for mode_file in sorted(drm.glob("card*-*/modes")):
                        status = mode_file.parent / "status"
                        if status.is_file() and status.read_text().strip() != "connected":
                            continue
                        modes = mode_file.read_text().splitlines()
                        if not modes:
                            continue
                        m = re.match(r"(\d+)x(\d+)(?:@(\d+))?", modes[0])
                        if m:
                            info.logical_width = _clamp_dim(m.group(1))
                            info.logical_height = _clamp_dim(m.group(2))
                            info.physical_width = info.logical_width
                            info.physical_height = info.logical_height
                            if m.group(3):
                                hz = _clamp_hz(m.group(3))
                                if hz:
                                    info.refresh_hz = hz
                            break
            except OSError:
                pass

    elif system == "windows":
        ps = which("powershell") or which("pwsh")
        if ps:
            ps_script = r"""
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class LclDisplay {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
    public short dmLogPixels;
    public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
    public int dmPanningWidth, dmPanningHeight;
  }
  [DllImport("user32.dll", CharSet=CharSet.Ansi)]
  public static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);
  [DllImport("user32.dll")] public static extern int GetDpiForSystem();
  public static string Info() {
    var dm = new DEVMODE();
    dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    if (!EnumDisplaySettings(null, -1, ref dm)) return "0 0 0 96";
    int dpi = 96;
    try { dpi = GetDpiForSystem(); } catch {}
    return dm.dmPelsWidth + " " + dm.dmPelsHeight + " " + dm.dmDisplayFrequency + " " + dpi;
  }
}
"@
[LclDisplay]::Info()
"""
            try:
                out = subprocess.check_output(
                    [ps, "-NoProfile", "-Command", ps_script],
                    text=True,
                    stderr=subprocess.DEVNULL,
                    timeout=15,
                )
                parts = out.strip().splitlines()[-1].split()
                if len(parts) >= 4:
                    info.logical_width = _clamp_dim(parts[0])
                    info.logical_height = _clamp_dim(parts[1])
                    info.physical_width = info.logical_width
                    info.physical_height = info.logical_height
                    hz = _clamp_hz(parts[2])
                    if hz:
                        info.refresh_hz = hz
                    dpi = float(parts[3])
                    if dpi > 0:
                        info.scale = _clamp_scale(dpi / 96.0)
            except (subprocess.CalledProcessError, FileNotFoundError, OSError, subprocess.TimeoutExpired):
                pass

    # Derive scale from physical/logical when possible (Retina etc.)
    if (
        info.scale == 1.0
        and info.physical_width
        and info.logical_width
        and info.logical_width > 0
        and info.physical_width != info.logical_width
    ):
        info.scale = _clamp_scale(info.physical_width / info.logical_width)

    # Default framebuffer target: logical points (sane QEMU window). Physical kept for scale.
    if info.logical_width and info.logical_height:
        info.width = info.logical_width
        info.height = info.logical_height
    elif info.physical_width and info.physical_height:
        info.width = info.physical_width
        info.height = info.physical_height

    return info


def detect_host_refresh_rate() -> int:
    return detect_host_display().refresh_hz


def launch_qemu(kernel: Path, native: bool = False) -> None:
    qemu = find_qemu()
    if not INITRAMFS_IMG.is_file():
        err(f"Missing initramfs: {INITRAMFS_IMG}")
        sys.exit(1)
    if not kernel.is_file():
        err(f"Missing kernel: {kernel}")
        sys.exit(1)

    memory = "2G"
    cpus = "2"
    host = detect_host_display()
    refresh_hz = host.refresh_hz

    if native:
        host_dpr = host.scale if host.scale > 0 else 1.0
        # Retina / HiDPI: guest FB = physical pixels, UI layout × DPR.
        # Logical FB + zoom-to-fit upscales → blurry/pixelated (not true Retina).
        if (
            host.physical_width
            and host.physical_height
            and host_dpr > 1.01
        ):
            width, height = host.physical_width, host.physical_height
            scale = host_dpr
        else:
            width, height = host.width, host.height
            scale = 1.0
    else:
        width, height = 1280, 800
        scale = 1.0
        host_dpr = 1.0

    kvm = Path("/dev/kvm")
    if kvm.exists() and os.access(kvm, os.R_OK | os.W_OK):
        accel = ["-enable-kvm", "-cpu", "host"]
    else:
        accel = ["-cpu", "max"]

    # Prefer Cocoa on macOS, SDL/GTK elsewhere when available
    gpu = ["-device", "virtio-vga-gl"]
    extra_qemu: list[str] = []
    if host_os() == "darwin":
        gpu = ["-device", "virtio-vga"]
        # zoom-to-fit: stretch guest FB to fill the screen (otherwise centered letterbox)
        if native:
            disp = "cocoa,full-screen=on,zoom-to-fit=on"
            extra_qemu = ["-full-screen"]
        else:
            disp = "cocoa"
        display = ["-display", disp]
    else:
        # Prefer gtk zoom-to-fit when available; else SDL fullscreen
        if native and which("qemu-system-x86_64"):
            # Probe is expensive; pick gtk if this QEMU build lists it via -display help
            try:
                help_out = subprocess.check_output(
                    [qemu, "-display", "help"],
                    text=True,
                    stderr=subprocess.STDOUT,
                )
            except (subprocess.CalledProcessError, FileNotFoundError):
                help_out = ""
            if "gtk" in help_out.splitlines() or "\ngtk" in help_out:
                disp = "gtk,gl=on,full-screen=on,zoom-to-fit=on"
            elif "sdl" in help_out:
                disp = "sdl,gl=on,full-screen=on"
            else:
                disp = "cocoa,full-screen=on,zoom-to-fit=on" if "cocoa" in help_out else "sdl"
            extra_qemu = ["-full-screen"]
        else:
            disp = "sdl,gl=on"
        display = ["-display", disp]

    print("----------------------------------------------------")
    print("  Launching QEMU Virtual Machine:")
    print(f"  - Memory: {memory}")
    print(f"  - SMP Cores: {cpus}")
    print(f"  - Guest video: {width}x{height}@{refresh_hz}")
    print(f"  - UI scale: {scale} (lcl.scale)")
    if native:
        print("  - Mode: native host display (fullscreen)")
        print(f"  - Host DPR: {host_dpr}  UI scale: {scale}")
        if host.logical_width and host.physical_width:
            print(
                f"  - Host logical: {host.logical_width}x{host.logical_height}  "
                f"physical: {host.physical_width}x{host.physical_height}"
            )
            if scale > 1.01:
                print("  - HiDPI: physical FB + scaled UI (sharp Retina path)")
    print(f"  - Accelerator: {' '.join(accel)}")
    print(f"  - GPU: {gpu[1]}")
    print(f"  - Display: {display[1]}")
    print("----------------------------------------------------")

    # Resolution -> DRM/KMS via kernel video= + lcl.width/height (DisplayManager picks mode)
    # lcl.scale multiplies UI only when FB is physical HiDPI (bare-metal); QEMU native uses 1.0
    video_mode = f"video={width}x{height}-32@{refresh_hz}"
    lcl_params = (
        f"lcl.scale={scale} lcl.width={width} lcl.height={height} "
        f"lcl.refresh={refresh_hz} lcl.dpr={host_dpr}"
    )
    if host.logical_width and host.logical_height:
        lcl_params += f" lcl.logical={host.logical_width}x{host.logical_height}"
    if host.physical_width and host.physical_height:
        lcl_params += f" lcl.physical={host.physical_width}x{host.physical_height}"

    append = (
        f"console=tty0 console=ttyS0,115200 {video_mode} {lcl_params} "
        f"earlyprintk=ttyS0 rdinit=/init quiet loglevel=3"
    )
    cmd = [
        qemu,
        *accel,
        "-kernel",
        str(kernel),
        "-initrd",
        str(INITRAMFS_IMG),
        "-append",
        append,
        "-m",
        memory,
        "-smp",
        cpus,
        *gpu,
        *display,
        *extra_qemu,
        "-usb",
        "-device",
        "usb-ehci,id=ehci",
        "-device",
        "usb-tablet,bus=ehci.0",
        "-device",
        "virtio-keyboard-pci",
        "-serial",
        "stdio",
    ]
    os.execvp(qemu, cmd)


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux QEMU launcher")
    parser.add_argument("--run", "-r", action="store_true", help="Launch QEMU after packaging")
    parser.add_argument("--build-only", action="store_true", help="Only build binaries")
    parser.add_argument("--package-only", action="store_true", help="Build + package initramfs (no QEMU)")
    parser.add_argument("--docker", action="store_true", help="Force Docker build/package path")
    parser.add_argument(
        "--native",
        "-n",
        action="store_true",
        help="Match host resolution + DPI scale (video= + lcl.scale cmdline)",
    )
    parser.add_argument(
        "--inside-docker",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args()

    print("====================================================")
    print("  LCL Core Linux - QEMU Isolated Boot Launcher      ")
    print("====================================================")

    # Inside Docker we always act as Linux native packager
    if args.inside_docker or (is_linux() and not args.docker):
        if args.build_only:
            cmake_build()
            return
        kernel = native_build_and_package()
        if args.run and not args.inside_docker:
            launch_qemu(
                kernel if isinstance(kernel, Path) else Path(kernel),
                native=args.native,
            )
        elif not args.inside_docker and not args.package_only and not args.build_only:
            # default without --run: prep only
            log("Boot environment ready!")
            log(f"Run '{Path(sys.argv[0]).name} --run' to launch QEMU in live VM.")
        elif args.package_only or args.inside_docker:
            log("Packaging complete.")
        return

    # macOS / Windows (or forced docker from non-package host entry)
    if args.build_only:
        build_only(force_docker=True)
        return

    kernel = prepare_artifacts(force_docker=args.docker or not is_linux())
    log(f"QEMU binary: {find_qemu()}")
    log(f"Kernel: {kernel}")

    if args.run:
        launch_qemu(kernel, native=args.native)
    else:
        log("Boot environment ready!")
        log(f"Run '{Path(sys.argv[0]).name} --run' to launch QEMU in live VM.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        err(f"Command failed with exit code {exc.returncode}: {exc.cmd}")
        sys.exit(exc.returncode or 1)
    except KeyboardInterrupt:
        sys.exit(130)
