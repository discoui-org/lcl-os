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
KERNEL_KVER_STAMP = CACHE_DIR / "kver"
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
    cache = BUILD_DIR / "CMakeCache.txt"
    if cache.is_file():
        try:
            content = cache.read_text(encoding="utf-8", errors="ignore")
            for line in content.splitlines():
                if line.startswith("CMAKE_CACHEFILE_DIR:INTERNAL="):
                    cached_dir = line.split("=", 1)[1].strip()
                    if cached_dir != str(BUILD_DIR.resolve()):
                        log(f"Cleaning stale CMakeCache.txt ({cached_dir} != {BUILD_DIR.resolve()})...")
                        shutil.rmtree(BUILD_DIR)
                        ensure_dirs()
                        break
        except Exception:
            pass
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


def _readable_kernel(path: Path) -> Path | None:
    try:
        resolved = path.resolve()
    except OSError:
        return None
    if resolved.is_file() and os.access(resolved, os.R_OK):
        return resolved
    return None


# --- Host kernel discovery DISABLED ---
# Packaging must never use the developer machine kernel/modules (CachyOS/Arch/etc.
# break guest DRM). All builds go through Docker (ubuntu:24.04 + linux-image-virtual).
#
# def find_host_kernel() -> Path | None:
#     ... intentionally removed; see git history if needed.


def locate_kernel() -> Path:
    """Find kernel+modules from the Docker builder image only.

    Called only with --inside-docker. Never reads the outer host OS kernel.
    """
    # Prefer cache written by a previous Docker package (same image kver)
    if (
        KERNEL_CACHE.is_file()
        and KERNEL_KVER_STAMP.is_file()
        and not os.environ.get("LCL_IGNORE_KERNEL_CACHE")
    ):
        kver = KERNEL_KVER_STAMP.read_text(encoding="utf-8").strip()
        mod = Path("/lib/modules") / kver
        cached_mod = CACHE_DIR / "modules" / kver
        if (mod / "kernel").is_dir() or (cached_mod / "kernel").is_dir():
            # Still prefer fresh /boot from this container when available
            pass

    # Ubuntu docker image: /boot/vmlinuz-* + matching /lib/modules/<kver>
    candidates: list[Path] = []
    boot = Path("/boot")
    if boot.is_dir():
        candidates.extend(sorted(boot.glob("vmlinuz-*"), reverse=True))
        candidates.append(Path("/boot/vmlinuz"))

    modules_root = Path("/lib/modules")
    if modules_root.is_dir():
        for entry in sorted(modules_root.iterdir(), reverse=True):
            candidates.append(entry / "vmlinuz")

    for path in candidates:
        if path.is_file() or path.is_symlink():
            found = _readable_kernel(path)
            if found:
                return found

    err("No kernel in Docker image. Rebuild: docker build -f scripts/Dockerfile.qemu")
    sys.exit(1)


def locate_module_tree(kernel_path: Path) -> tuple[Path, str]:
    """Return (modules_dir, kver) matching the kernel we will boot."""
    resolved = kernel_path.resolve()

    # Cached kernel with stamp
    if (
        KERNEL_CACHE.is_file()
        and resolved == KERNEL_CACHE.resolve()
        and KERNEL_KVER_STAMP.is_file()
    ):
        kver = KERNEL_KVER_STAMP.read_text(encoding="utf-8").strip()
        for mod in (Path("/lib/modules") / kver, CACHE_DIR / "modules" / kver):
            if (mod / "kernel").is_dir():
                return mod, kver

    # vmlinuz-6.8.0-51-generic -> 6.8.0-51-generic
    name = kernel_path.name
    if name.startswith("vmlinuz-"):
        kver = name[len("vmlinuz-") :]
        mod = Path("/lib/modules") / kver
        if (mod / "kernel").is_dir():
            return mod, kver

    # /lib/modules/<kver>/vmlinuz
    parent = kernel_path.parent
    if parent.name and (parent / "kernel").is_dir():
        return parent, parent.name

    # Running host (native Linux package path)
    uname_r = platform.release()
    host_mod = Path("/lib/modules") / uname_r
    if (host_mod / "kernel").is_dir():
        return host_mod, uname_r

    # Docker image: only modules that exist (never mix random newest tree)
    modules_root = Path("/lib/modules")
    if modules_root.is_dir():
        for entry in sorted(modules_root.iterdir(), reverse=True):
            if (entry / "kernel").is_dir():
                return entry, entry.name

    err(f"No module tree found for kernel {kernel_path}")
    sys.exit(1)


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
    log(f"Kernel modules source: {kmod_base} (kver={kver})")

    target = init_root / "usr" / "lib" / "modules" / kver
    # Always refresh modules for this package run (avoid stale empty trees)
    if target.exists():
        shutil.rmtree(target)

    log(f"Packaging DRM & input kernel modules ({kver})...")
    target.mkdir(parents=True, exist_ok=True)
    copied_any = False
    for rel in (
        "kernel/drivers/gpu/drm",
        "kernel/drivers/virtio",
        "kernel/drivers/gpu/drm/virtio",
        "kernel/drivers/hid",
        "kernel/drivers/input",
        "kernel/drivers/virtio",
        "kernel/drivers/char/virtio_console",
    ):
        src = kmod_base / rel
        if src.is_dir():
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists():
                shutil.rmtree(dst)
            shutil.copytree(src, dst, symlinks=False)
            copied_any = True
        elif src.is_file():
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst, follow_symlinks=True)
            copied_any = True

    # Also pull any virtio_gpu*.ko* wherever it lives
    for pattern in ("**/virtio_gpu.ko*", "**/virtio-gpu.ko*", "**/drm.ko*", "**/virtio_pci.ko*"):
        for src in kmod_base.glob(pattern):
            rel = src.relative_to(kmod_base)
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst, follow_symlinks=True)
            copied_any = True

    for zst in list(target.rglob("*.ko.zst")):
        try:
            run(["zstd", "-d", "--rm", str(zst)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            pass

    # Copy modules.builtin / modules.order if present (helps depmod)
    for meta in ("modules.builtin", "modules.builtin.modinfo", "modules.order", "modules.dep", "modules.alias"):
        src_meta = kmod_base / meta
        if src_meta.is_file():
            shutil.copy2(src_meta, target / meta, follow_symlinks=True)

    if which("depmod"):
        try:
            run(
                ["depmod", "-b", str(init_root), kver],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            pass

    has_virtio_gpu = any(target.rglob("virtio_gpu.ko*")) or any(target.rglob("virtio-gpu.ko*"))
    builtin_txt = ""
    builtin_path = kmod_base / "modules.builtin"
    if builtin_path.is_file():
        builtin_txt = builtin_path.read_text(encoding="utf-8", errors="ignore")
        shutil.copy2(builtin_path, target / "modules.builtin", follow_symlinks=True)
    virtio_builtin = "virtio_gpu" in builtin_txt or "virtio-gpu" in builtin_txt

    if not copied_any and not virtio_builtin:
        err(f"No kernel modules copied from {kmod_base}")
        sys.exit(1)
    if has_virtio_gpu:
        log("virtio_gpu module packaged OK")
    elif virtio_builtin:
        log("virtio_gpu is built-in to this kernel (no .ko needed)")
    else:
        log(f"WARNING: virtio_gpu not found under {kmod_base} — DRM may be unavailable")

    # Persist matching cache pair
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    KERNEL_KVER_STAMP.write_text(kver + "\n", encoding="utf-8")
    cache_mod = CACHE_DIR / "modules" / kver
    if cache_mod.exists():
        shutil.rmtree(cache_mod)
    shutil.copytree(target, cache_mod)


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

    # Tools from the Docker image only (never host OS userland)
    host_bins = {
        "sh": Path("/bin/sh"),
        "bash": Path(which("bash") or "/usr/bin/bash"),
        "mount": Path("/bin/mount"),
        "mkdir": Path("/bin/mkdir"),
        "sleep": Path("/bin/sleep"),
        "ls": Path("/bin/ls"),
        "cat": Path("/bin/cat"),
        "uname": Path("/usr/bin/uname"),
        "grep": Path("/usr/bin/grep"),
        "printf": Path("/usr/bin/printf"),
        "modprobe": Path("/sbin/modprobe"),
        "depmod": Path("/sbin/depmod"),
        "kmod": Path("/bin/kmod"),
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

    # Package Mesa DRI and GBM drivers for EGL hardware acceleration
    log("Packaging Mesa DRI and GBM graphics drivers...")
    dri_dirs = [
        Path("/usr/lib/x86_64-linux-gnu/dri"),
        Path("/usr/lib/dri"),
        Path("/usr/lib/x86_64-linux-gnu/gbm"),
        Path("/usr/lib/gbm"),
    ]
    for dri_src in dri_dirs:
        if dri_src.is_dir():
            dri_dst = INITRAMFS_DIR / "usr" / "lib" / "x86_64-linux-gnu" / dri_src.name
            dri_dst.mkdir(parents=True, exist_ok=True)
            for item in dri_src.iterdir():
                if item.is_file():
                    shutil.copy2(item, dri_dst / item.name, follow_symlinks=True)
                    copy_ldd_deps(item, dest_lib)

    package_kernel_modules(kernel_path, INITRAMFS_DIR)

    # Cache kernel next to artifacts when packaging inside Docker/Linux
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    resolved = kernel_path.resolve()
    if resolved != KERNEL_CACHE.resolve():
        shutil.copy2(resolved, KERNEL_CACHE, follow_symlinks=True)
        try:
            KERNEL_CACHE.chmod(0o644)
        except Exception:
            pass

    write_text(
        INITRAMFS_DIR / "init",
        """#!/bin/sh
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/input /home/user/Desktop /home/user/Documents /home/user/Downloads /home/user/Applications
mount -t devpts devpts /dev/pts -o mode=0620,ptmxmode=0666 2>/dev/null || mount -t devpts devpts /dev/pts 2>/dev/null || true
if [ ! -e /dev/ptmx ]; then
    mknod -m 666 /dev/ptmx c 5 2 2>/dev/null || ln -sf pts/ptmx /dev/ptmx 2>/dev/null || true
fi
chmod 666 /dev/ptmx 2>/dev/null || true

# Resolve module tree (uname -r inside guest matches booted kernel)
KVER=$(uname -r 2>/dev/null)
if [ -d "/lib/modules/$KVER" ]; then
    echo "[init] modules: /lib/modules/$KVER"
else
    echo "[init] WARNING: /lib/modules/$KVER missing"
    ls -la /lib/modules 2>/dev/null || true
fi

# Load GPU / input stack BEFORE creating any device nodes (let devtmpfs populate)
modprobe virtio_pci || true
modprobe virtio_dma_buf || true
modprobe drm || true
modprobe drm_kms_helper || true
modprobe virtio_gpu || true
modprobe bochs || true
modprobe simpledrm || true
modprobe virtio_input || true
modprobe usbhid || true
modprobe hid_generic || true
modprobe evdev || true
modprobe qemu_fw_cfg || true

# Wait for /dev/dri/card* (up to ~3s)
i=0
while [ "$i" -lt 30 ]; do
    if ls /dev/dri/card* >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

echo "===================================================="
echo "  LCL Core Linux (LCL) - QEMU Direct Kernel Boot   "
echo "===================================================="
echo "Kernel: $(uname -r)  cmdline: $(cat /proc/cmdline 2>/dev/null)"
echo "DRM devices detected:"
ls -la /dev/dri/ 2>/dev/null || echo "  (none)"
echo "Input devices detected:"
ls /dev/input/ 2>/dev/null || echo "  (none yet)"
echo "Loaded drm-related modules:"
cat /proc/modules 2>/dev/null | grep -E 'virtio|drm|bochs' || echo "  (none)"
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
    """Build/rebuild image when missing or Dockerfile.qemu changed."""
    log(f"Ensuring Docker image {DOCKER_IMAGE}...")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    stamp = CACHE_DIR / "docker-image.stamp"
    df_mtime = str(DOCKERFILE.stat().st_mtime)

    inspect = subprocess.run(
        ["docker", "image", "inspect", DOCKER_IMAGE],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    need_build = inspect.returncode != 0
    if not need_build and stamp.is_file():
        need_build = stamp.read_text(encoding="utf-8").strip() != df_mtime
    elif not need_build and not stamp.is_file():
        need_build = True

    if need_build:
        log("Building Docker builder image (first run / Dockerfile change may take a few minutes)...")
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
        stamp.write_text(df_mtime + "\n", encoding="utf-8")


def fix_permissions() -> None:
    """Fix ownership and permissions on /src/build inside Docker so host user can read/delete."""
    if not BUILD_DIR.exists():
        return
    try:
        run(["chmod", "-R", "a+rwX", str(BUILD_DIR)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass
    uid = os.environ.get("HOST_UID")
    gid = os.environ.get("HOST_GID")
    if uid and gid:
        try:
            run(["chown", "-R", f"{uid}:{gid}", str(BUILD_DIR)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass


def clean_build() -> None:
    """Safely clean build directory using host or Docker root fallback."""
    if not BUILD_DIR.exists():
        log("build/ directory does not exist. Nothing to clean.")
        return
    log("Cleaning build/ directory...")
    try:
        shutil.rmtree(BUILD_DIR)
        log("Successfully removed build/")
    except Exception:
        if docker_available():
            log("Removing build/ via Docker container...")
            mount = str(ROOT_DIR)
            run(["docker", "run", "--rm", "-v", f"{mount}:/src", DOCKER_IMAGE, "rm", "-rf", "/src/build"])
            log("Successfully removed build/ via Docker")
        else:
            err("Failed to remove build/. Try running: sudo rm -rf build")
            sys.exit(1)


def docker_run(args: list[str]) -> None:
    ensure_docker_image()
    # Resolve Windows paths for Docker Desktop if needed
    mount = str(ROOT_DIR)
    env_args: list[str] = []
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        env_args = ["-e", f"HOST_UID={os.getuid()}", "-e", f"HOST_GID={os.getgid()}"]
    cmd = [
        "docker",
        "run",
        "--rm",
        "--platform",
        "linux/amd64",
        *env_args,
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


def docker_build_and_package() -> Path:
    """Build + package exclusively inside Docker (ubuntu amd64)."""
    ensure_dirs()
    if not docker_available():
        err("Docker is required on every host (Linux/macOS/Windows).")
        err("Install Docker, then: docker build -f scripts/Dockerfile.qemu -t lcl-os-qemu-builder:latest scripts/")
        sys.exit(1)
    docker_run(["--package-only"])
    if not KERNEL_CACHE.is_file() or not INITRAMFS_IMG.is_file():
        err("Docker packaging did not produce kernel/initramfs artifacts.")
        sys.exit(1)
    return KERNEL_CACHE


def package_inside_docker() -> Path:
    """Runs only with --inside-docker: cmake + initramfs from image kernel/modules."""
    ensure_dirs()
    cmake_build()
    kernel = locate_kernel()
    log(f"Docker image kernel: {kernel}")
    package_initramfs(kernel)
    res = KERNEL_CACHE if KERNEL_CACHE.is_file() else kernel
    fix_permissions()
    return res


# --- Host-native package path DISABLED (do not use outer OS kernel/modules) ---
# def native_build_and_package() -> Path:
#     ensure_dirs()
#     cmake_build()
#     kernel = locate_kernel()  # used to pick CachyOS/host vmlinuz — broken for guest DRM
#     package_initramfs(kernel)
#     return KERNEL_CACHE


def prepare_artifacts() -> Path:
    """Always Docker — never package from the host OS."""
    return docker_build_and_package()


def build_only() -> None:
    """Always Docker — never compile against host libdrm/headers."""
    ensure_dirs()
    if not docker_available():
        err("Docker is required to build on every host.")
        sys.exit(1)
    docker_run(["--build-only"])
    log(f"Linux binaries in {BUILD_DIR} (produced via Docker)")


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


def validate_kernel(kernel: Path) -> None:
    """Ensure kernel is readable and looks like a bzImage QEMU can -kernel boot."""
    if not kernel.is_file():
        err(f"Missing kernel: {kernel}")
        sys.exit(1)
    if not os.access(kernel, os.R_OK):
        err(f"Kernel not readable (try sudo or copy to build/qemu-cache): {kernel}")
        sys.exit(1)
    try:
        data = kernel.read_bytes()[:512]
    except OSError as exc:
        err(f"Cannot read kernel: {exc}")
        sys.exit(1)
    # bzImage has "HdrS" at offset 0x202 in the setup header
    if len(data) >= 0x206 and data[0x202:0x206] != b"HdrS":
        # Also accept raw files identified by `file` later; warn only
        log(f"WARNING: {kernel} may not be a bzImage (HdrS missing). QEMU -kernel might fail.")


def launch_qemu(kernel: Path, native: bool = False) -> None:
    qemu = find_qemu()
    if not INITRAMFS_IMG.is_file():
        err(f"Missing initramfs: {INITRAMFS_IMG}")
        sys.exit(1)
    validate_kernel(kernel)

    memory = "2G"
    cpus = "2"
    host = detect_host_display()
    refresh_hz = host.refresh_hz

    if native:
        host_dpr = host.scale if host.scale > 0 else 1.0
        logical_w = host.logical_width or host.width
        logical_h = host.logical_height or host.height
        physical_w = host.physical_width or logical_w
        physical_h = host.physical_height or logical_h
        # True Retina only when physical pixels > logical points.
        # GNOME scaling-factor (e.g. 4) with physical==logical is NOT FB HiDPI —
        # using scale=4 there makes a tiny/broken UI and mis-labels the path.
        true_retina = (
            physical_w > 0
            and logical_w > 0
            and physical_w >= int(logical_w * 1.5)
        )
        if true_retina:
            width, height = physical_w, physical_h
            scale = host_dpr if host_dpr > 1.01 else float(physical_w) / float(logical_w)
        else:
            width, height = logical_w, logical_h
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

    want_gl = os.environ.get("LCL_QEMU_GL", "").lower() in ("1", "true", "yes", "on")

    # Probe available display backends once
    try:
        disp_help = subprocess.check_output(
            [qemu, "-display", "help"],
            text=True,
            stderr=subprocess.STDOUT,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        disp_help = ""
    backends = {line.strip() for line in disp_help.splitlines() if line.strip()}

    extra_qemu: list[str] = []
    # Always disable default stdvga when attaching virtio-vga (avoids dual-head / stuck BIOS fb)
    # GL path is opt-in: virtio-vga-gl + gl=on often hangs on Linux ("Booting from ROM..." freeze).
    if want_gl:
        gpu = ["-vga", "none", "-device", "virtio-vga-gl"]
    else:
        gpu = ["-vga", "none", "-device", "virtio-vga"]

    if host_os() == "darwin":
        if native:
            disp = "cocoa,full-screen=on,zoom-to-fit=on"
            extra_qemu = ["-full-screen"]
        else:
            disp = "cocoa"
        display = ["-display", disp]
    else:
        # Linux: prefer software path (no gl) — reliable DRM/KMS in guest
        if "gtk" in backends:
            if native:
                disp = "gtk,full-screen=on,zoom-to-fit=on"
                if want_gl:
                    disp = "gtk,gl=on,full-screen=on,zoom-to-fit=on"
                extra_qemu = ["-full-screen"]
            else:
                disp = "gtk,gl=on" if want_gl else "gtk,zoom-to-fit=on"
        elif "sdl" in backends:
            if native:
                disp = "sdl,full-screen=on"
                if want_gl:
                    disp = "sdl,gl=on,full-screen=on"
                extra_qemu = ["-full-screen"]
            else:
                disp = "sdl,gl=on" if want_gl else "sdl"
        else:
            disp = "curses"
            log("No gtk/sdl display backend; falling back to curses (install qemu gtk/sdl)")
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
    print(f"  - GPU: {' '.join(gpu)}{'  (LCL_QEMU_GL=1 for VirGL)' if not want_gl else ''}")
    print(f"  - Display: {display[1]}")
    print(f"  - Kernel: {kernel}")
    print(f"  - Initrd: {INITRAMFS_IMG}")
    print("----------------------------------------------------")

    # Resolution -> DRM/KMS via kernel video= + lcl.width/height (DisplayManager picks mode)
    video_mode = f"video={width}x{height}-32@{refresh_hz}"
    lcl_params = (
        f"lcl.scale={scale} lcl.width={width} lcl.height={height} "
        f"lcl.refresh={refresh_hz} lcl.dpr={host_dpr}"
    )
    if host.logical_width and host.logical_height:
        lcl_params += f" lcl.logical={host.logical_width}x{host.logical_height}"
    if host.physical_width and host.physical_height:
        lcl_params += f" lcl.physical={host.physical_width}x{host.physical_height}"

    # Keep early messages on tty0 so a hung GPU still shows progress (not frozen SeaBIOS text).
    # Serial mirrors the same log on the host terminal.
    append = (
        f"console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200 "
        f"{video_mode} {lcl_params} "
        f"rdinit=/init loglevel=6"
    )
    cmd = [
        qemu,
        *accel,
        "-machine",
        "q35",
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
        "-device",
        "virtio-keyboard-pci",
        "-device",
        "virtio-tablet-pci",
        "-serial",
        "stdio",
        "-no-reboot",
    ]
    log("QEMU cmdline: " + " ".join(cmd))
    os.execvp(qemu, cmd)


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux QEMU launcher")
    parser.add_argument("--run", "-r", action="store_true", help="Launch QEMU after packaging")
    parser.add_argument("--build-only", action="store_true", help="Only build binaries (Docker)")
    parser.add_argument("--package-only", action="store_true", help="Build + package initramfs (no QEMU)")
    parser.add_argument("--clean", action="store_true", help="Remove build/ directory safely")
    parser.add_argument(
        "--docker",
        action="store_true",
        help="Ignored (Docker is always used for build/package)",
    )
    parser.add_argument(
        "--native",
        "-n",
        action="store_true",
        help="Match host resolution + DPI scale (video= + lcl.scale cmdline)",
    )
    parser.add_argument(
        "--gpu",
        "-g",
        action="store_true",
        help="Enable 3D VirGL GPU acceleration in QEMU",
    )
    parser.add_argument(
        "--inside-docker",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    args = parser.parse_args()

    if args.gpu:
        os.environ["LCL_QEMU_GL"] = "1"

    if args.clean:
        clean_build()
        return

    print("====================================================")
    print("  LCL Core Linux - QEMU Isolated Boot Launcher      ")
    print("====================================================")

    # ---- Inside Docker image: compile + package with image kernel/modules only ----
    if args.inside_docker:
        if args.build_only:
            cmake_build()
            log("Docker build-only complete.")
            return
        kernel = package_inside_docker()
        log(f"Packaging complete. Kernel cache: {kernel}")
        return

    # ---- Outer host (Linux/macOS/Windows): always Docker for artifacts, QEMU on host ----
    # Host OS kernel/modules/userland are NEVER used for the guest initramfs.
    if args.build_only:
        build_only()
        return

    kernel = prepare_artifacts()
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
