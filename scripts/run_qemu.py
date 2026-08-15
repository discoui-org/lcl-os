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
SESSIOND_BIN = BUILD_DIR / "lcl-sessiond"
SHELL_BIN = BUILD_DIR / "lcl-desktop-shell"
TERM_BIN = BUILD_DIR / "lcl-terminal"
DEMO_BIN = BUILD_DIR / "apps" / "ui_demo" / "lcl_ui_demo"
JS_BIN = BUILD_DIR / "lcl-js"
DEMO_JS_DIR = BUILD_DIR / "apps" / "ui_demo_js"
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


def docker_platform(arch: str) -> str:
    return "linux/arm64" if arch == "aarch64" else "linux/amd64"


def docker_image_name(arch: str) -> str:
    return f"lcl-os-qemu-builder:{arch}"


def find_qemu(arch: str = "x86_64") -> str:
    binary_name = "qemu-system-aarch64" if arch == "aarch64" else "qemu-system-x86_64"

    # 1. Check explicit environment override
    override = os.environ.get("LCL_QEMU_BIN")
    if override and Path(override).is_file():
        log(f"Using custom QEMU binary (LCL_QEMU_BIN): {override}")
        return override

    # 2. On macOS, prioritize UTM QEMU binary (includes Metal VirGL 3D acceleration)
    if host_os() == "darwin":
        utm_candidates = [
            Path("/Applications/UTM.app/Contents/MacOS") / binary_name,
            Path.home() / "Applications" / "UTM.app" / "Contents" / "MacOS" / binary_name,
            Path("/Applications/UTM.app/Contents/Resources") / binary_name,
        ]
        for utm_bin in utm_candidates:
            if utm_bin.is_file() and os.access(utm_bin, os.X_OK):
                log(f"Using UTM QEMU binary (Hardware Accelerated): {utm_bin}")
                return str(utm_bin)

    # 3. System PATH QEMU binary
    qemu = which(binary_name)
    if qemu:
        return qemu

    # 4. Common Homebrew / system fallback paths
    fallback_paths = [
        Path(f"/opt/homebrew/bin/{binary_name}"),
        Path(f"/usr/local/bin/{binary_name}"),
        Path(f"/usr/bin/{binary_name}"),
    ]
    for p in fallback_paths:
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)

    err(f"{binary_name} is not installed.")
    if host_os() == "darwin":
        err("To run standalone CLI QEMU: brew install qemu")
        err("Or run with UTM: ./main.py utm")
    elif host_os() == "windows":
        err(f"Install QEMU and ensure {binary_name} is on PATH.")
    else:
        err(f"Install {binary_name} via your package manager.")
    sys.exit(1)


def ensure_fonts() -> None:
    inter = ROOT_DIR / "assets" / "fonts" / "inter"
    if not inter.is_dir() or not any(inter.iterdir()):
        fetch = SCRIPT_DIR / "fetch_fonts.py"
        if fetch.is_file():
            log("Font assets missing. Executing fetch_fonts.py...")
            run([sys.executable, str(fetch)])


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

    log(f"Packaging DRM, USB, I2C, touchpad & input kernel modules ({kver})...")
    target.mkdir(parents=True, exist_ok=True)
    copied_any = False
    for rel in (
        "kernel/drivers/gpu/drm",
        "kernel/drivers/virtio",
        "kernel/drivers/gpu/drm/virtio",
        "kernel/drivers/hid",
        "kernel/drivers/input",
        "kernel/drivers/usb",
        "kernel/drivers/i2c",
        "kernel/drivers/pinctrl",
        "kernel/drivers/spi",
        "kernel/drivers/platform",
        "kernel/drivers/acpi",
        "kernel/drivers/bus",
        "kernel/drivers/mfd",
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


def package_initramfs(kernel_path: Path, *, trace_frames: bool = False,
                      debug_layout: bool = False, debug_overlay: bool = False) -> None:
    log("Building canonical LCL rootfs ext4 image...")
    from build_rootfs import build_rootfs_ext4
    build_rootfs_ext4(arch="x86_64")

    log("Preparing minimal bootstrap initramfs root directory structure...")
    if INITRAMFS_DIR.exists():
        shutil.rmtree(INITRAMFS_DIR)

    for sub in (
        "proc",
        "sys",
        "dev",
        "tmp",
        "etc",
        "var/log",
        "sysroot",
        "usr/bin",
        "usr/lib",
        "usr/sbin",
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

    def find_host_bin(name: str) -> Path | None:
        p = which(name)
        if p:
            found = Path(p)
            if found.is_file():
                return found
        for cand in (f"/usr/bin/{name}", f"/bin/{name}", f"/usr/sbin/{name}", f"/sbin/{name}"):
            cp = Path(cand)
            if cp.is_file():
                return cp
        return None

    bootstrap_tools = [
        "switch_root", "sh", "bash", "mount", "mkdir", "sleep",
        "modprobe", "depmod", "kmod", "dmesg"
    ]

    log("Packaging minimal bootstrap utilities into initramfs...")
    dest_bin = INITRAMFS_DIR / "usr" / "bin"
    dest_lib = INITRAMFS_DIR / "usr" / "lib"

    for name in bootstrap_tools:
        src = find_host_bin(name)
        if src and src.is_file():
            shutil.copy2(src, dest_bin / name, follow_symlinks=True)
            (dest_bin / name).chmod(0o755)
            copy_ldd_deps(dest_bin / name, dest_lib)

    # Dynamic linker
    for loader_cand in [
        Path("/lib64/ld-linux-x86-64.so.2"),
        Path("/usr/lib64/ld-linux-x86-64.so.2"),
        Path("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
        Path("/usr/lib/ld-linux-x86-64.so.2"),
    ]:
        if loader_cand.is_file():
            shutil.copy2(loader_cand.resolve(), dest_lib / "ld-linux-x86-64.so.2")
            break

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
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

# Load storage and display modules early
modprobe virtio_pci 2>/dev/null || true
modprobe virtio_blk 2>/dev/null || true
modprobe virtio_gpu 2>/dev/null || true
modprobe virtio_input 2>/dev/null || true
modprobe ext4 2>/dev/null || true

# Auto-probe hardware drivers via /sys modaliases
for alias_file in $(find /sys/bus /sys/devices -name modalias 2>/dev/null); do
    alias=$(cat "$alias_file" 2>/dev/null)
    if [ -n "$alias" ]; then
        modprobe $alias 2>/dev/null || true
    fi
done

# Wait for root block device
ROOT_DEV="/dev/vda"
i=0
while [ "$i" -lt 60 ]; do
    if [ -b "/dev/vda" ]; then
        ROOT_DEV="/dev/vda"
        break
    elif [ -b "/dev/disk/by-label/lcl-rootfs" ]; then
        ROOT_DEV="/dev/disk/by-label/lcl-rootfs"
        break
    fi
    i=$((i + 1))
    sleep 0.05
done

mkdir -p /sysroot
echo "[bootstrap] Mounting canonical LCL rootfs from $ROOT_DEV on /sysroot..."
mount -t ext4 -o rw,noatime $ROOT_DEV /sysroot 2>/dev/null || mount -t ext4 $ROOT_DEV /sysroot

if [ ! -x /sysroot/init ] && [ ! -x /sysroot/usr/bin/lcl-sessiond ]; then
    echo "[bootstrap ERROR] Could not find canonical /init on $ROOT_DEV!"
    exec /bin/sh
fi

# Forward kernel module tree to rootfs if not already populated
KVER=$(uname -r 2>/dev/null)
if [ -d "/lib/modules/$KVER" ] && [ ! -d "/sysroot/usr/lib/modules/$KVER" ]; then
    mkdir -p "/sysroot/usr/lib/modules" 2>/dev/null
    mount --bind "/lib/modules/$KVER" "/sysroot/usr/lib/modules/$KVER" 2>/dev/null || \
    cp -a "/lib/modules/$KVER" "/sysroot/usr/lib/modules/" 2>/dev/null || true
fi

# Move pseudo filesystems to sysroot before switch_root
mkdir -p /sysroot/run /sysroot/dev /sysroot/proc /sysroot/sys
if mountpoint -q /run 2>/dev/null; then
    mount --move /run /sysroot/run 2>/dev/null || true
fi
mount --move /dev /sysroot/dev 2>/dev/null || true
mount --move /proc /sysroot/proc 2>/dev/null || true
mount --move /sys /sysroot/sys 2>/dev/null || true

echo "[bootstrap] Switching root to canonical LCL rootfs..."
exec switch_root /sysroot /init
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


def ensure_docker_image(arch: str = "x86_64") -> None:
    """Build/rebuild image when missing or Dockerfile.qemu changed."""
    image_tag = docker_image_name(arch)
    plat = docker_platform(arch)
    log(f"Ensuring Docker image {image_tag} ({plat})...")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    stamp = CACHE_DIR / f"docker-image-{arch}.stamp"
    df_mtime = str(DOCKERFILE.stat().st_mtime)

    inspect = subprocess.run(
        ["docker", "image", "inspect", image_tag],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    need_build = inspect.returncode != 0
    if not need_build and stamp.is_file():
        need_build = stamp.read_text(encoding="utf-8").strip() != df_mtime
    elif not need_build and not stamp.is_file():
        need_build = True

    if need_build:
        log(f"Building Docker builder image for {arch} ({plat}) (first run / Dockerfile change may take a few minutes)...")
        run(
            [
                "docker",
                "build",
                "--platform",
                plat,
                "-t",
                image_tag,
                "-f",
                str(DOCKERFILE),
                str(SCRIPT_DIR),
            ]
        )
        # CACHE_DIR may be root-owned from a previous Docker run; try to fix
        # permissions before writing the stamp so the host user can write it.
        try:
            subprocess.run(
                ["chmod", "-R", "a+rwX", str(CACHE_DIR)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            pass
        try:
            stamp.write_text(df_mtime + "\n", encoding="utf-8")
        except PermissionError:
            log(
                f"[WARN] Could not write stamp {stamp} (permission denied). "
                "Run: sudo chown -R $USER:$USER build/  to fix ownership. "
                "The Docker image was built successfully and QEMU will proceed."
            )


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


def clean_build(arch: str = "x86_64") -> None:
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
            run(["docker", "run", "--rm", "-v", f"{mount}:/src", docker_image_name(arch), "rm", "-rf", "/src/build"])
            log("Successfully removed build/ via Docker")
        else:
            err("Failed to remove build/. Try running: sudo rm -rf build")
            sys.exit(1)


def docker_run(args: list[str], arch: str = "x86_64") -> None:
    ensure_docker_image(arch)
    image_tag = docker_image_name(arch)
    plat = docker_platform(arch)
    mount = str(ROOT_DIR)
    env_args: list[str] = []
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        env_args = ["-e", f"HOST_UID={os.getuid()}", "-e", f"HOST_GID={os.getgid()}"]
    cmd = [
        "docker",
        "run",
        "--rm",
        "--platform",
        plat,
        *env_args,
        "-v",
        f"{mount}:/src",
        "-w",
        "/src",
        image_tag,
        "python3",
        "scripts/run_qemu.py",
        *args,
        "--arch",
        arch,
        "--inside-docker",
    ]
    log(f"Running build/package inside Docker ({plat})...")
    run(cmd)


def docker_build_and_package(diagnostic_args: list[str], arch: str = "x86_64") -> Path:
    """Build + package exclusively inside Docker."""
    ensure_dirs()
    if not docker_available():
        err("Docker is required on every host (Linux/macOS/Windows).")
        err(f"Install Docker, then: docker build --platform {docker_platform(arch)} -f scripts/Dockerfile.qemu -t {docker_image_name(arch)} scripts/")
        sys.exit(1)
    docker_run(["--package-only", *diagnostic_args], arch=arch)
    if not KERNEL_CACHE.is_file() or not INITRAMFS_IMG.is_file():
        err("Docker packaging did not produce kernel/initramfs artifacts.")
        sys.exit(1)
    return KERNEL_CACHE


def package_inside_docker(args: argparse.Namespace) -> Path:
    """Runs only with --inside-docker: cmake + initramfs from image kernel/modules."""
    ensure_dirs()
    cmake_build()
    kernel = locate_kernel()
    log(f"Docker image kernel: {kernel}")
    package_initramfs(
        kernel,
        trace_frames=args.trace_frames,
        debug_layout=args.debug_layout,
        debug_overlay=args.debug_overlay,
    )
    res = KERNEL_CACHE if KERNEL_CACHE.is_file() else kernel
    fix_permissions()
    return res


def prepare_artifacts(args: argparse.Namespace, arch: str = "x86_64") -> Path:
    """Always Docker — never package from the host OS."""
    diagnostic_args = []
    if args.trace_frames:
        diagnostic_args.append("--trace-frames")
    if args.debug_layout:
        diagnostic_args.append("--debug-layout")
    if args.debug_overlay:
        diagnostic_args.append("--debug-overlay")
    return docker_build_and_package(diagnostic_args, arch=arch)


def build_only(arch: str = "x86_64") -> None:
    """Always Docker — never compile against host libdrm/headers."""
    ensure_dirs()
    if not docker_available():
        err("Docker is required to build on every host.")
        sys.exit(1)
    docker_run(["--build-only"], arch=arch)
    log(f"Linux ({arch}) binaries in {BUILD_DIR} (produced via Docker)")


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
                if not info.refresh_hz:
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


def find_ovmf_firmware(arch: str = "x86_64") -> Path | None:
    if arch == "aarch64":
        candidates = [
            Path("/Applications/UTM.app/Contents/Resources/qemu/edk2-aarch64-code.fd"),
            Path("/Applications/UTM.app/Contents/Resources/edk2-aarch64-code.fd"),
            Path.home() / "Applications" / "UTM.app" / "Contents" / "Resources" / "qemu" / "edk2-aarch64-code.fd",
            Path("/opt/homebrew/share/qemu/edk2-aarch64-code.fd"),
            Path("/usr/local/share/qemu/edk2-aarch64-code.fd"),
            Path("/usr/share/qemu-efi-aarch64/QEMU_EFI.fd"),
            Path("/usr/share/edk2/aarch64/QEMU_EFI.fd"),
            Path("/usr/share/AAVMF/AAVMF_CODE.fd"),
            Path("/usr/share/qemu/edk2-aarch64-code.fd"),
        ]
    else:
        candidates = [
            Path("/Applications/UTM.app/Contents/Resources/qemu/edk2-x86_64-code.fd"),
            Path("/Applications/UTM.app/Contents/Resources/edk2-x86_64-code.fd"),
            Path.home() / "Applications" / "UTM.app" / "Contents" / "Resources" / "qemu" / "edk2-x86_64-code.fd",
            Path("/opt/homebrew/share/qemu/edk2-x86_64-code.fd"),
            Path("/usr/local/share/qemu/edk2-x86_64-code.fd"),
            Path("/usr/share/edk2/x64/OVMF_CODE.4m.fd"),
            Path("/usr/share/edk2/x64/OVMF_CODE.fd"),
            Path("/usr/share/OVMF/OVMF_CODE.fd"),
            Path("/usr/share/ovmf/OVMF.fd"),
            Path("/usr/share/qemu/OVMF.fd"),
            Path("/usr/share/edk2/x64/OVMF.4m.fd"),
        ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def launch_qemu(
    kernel: Path,
    arch: str = "x86_64",
    native: bool = False,
    iso_mode: bool = False,
    uefi_mode: bool = False,
    usb_passthrough: str | None = None,
    retina: bool = False,
    scale_override: float | None = None,
    width_override: int | None = None,
    height_override: int | None = None,
) -> None:
    qemu = find_qemu(arch)
    host = detect_host_display()

    memory = os.environ.get("LCL_QEMU_MEM") or "2G"
    cpus = os.environ.get("LCL_QEMU_CPUS", "2")
    refresh_hz = host.refresh_hz or 60

    env_scale = os.environ.get("SCALE") or os.environ.get("LCL_SCALE")
    if scale_override is None and env_scale:
        try:
            scale_override = float(env_scale)
        except ValueError:
            pass

    env_width = os.environ.get("WIDTH") or os.environ.get("LCL_WIDTH")
    if width_override is None and env_width:
        try:
            width_override = int(env_width)
        except ValueError:
            pass

    env_height = os.environ.get("HEIGHT") or os.environ.get("LCL_HEIGHT")
    if height_override is None and env_height:
        try:
            height_override = int(env_height)
        except ValueError:
            pass

    if retina:
        # 13" MacBook Air Retina baseline: 2560x1600 physical resolution, 2.0x UI scale (1280x800 logical viewport)
        width = width_override or 2560
        height = height_override or 1600
        scale = scale_override or 2.0
        host_dpr = scale
        host.logical_width = width // 2
        host.logical_height = height // 2
        host.physical_width = width
        host.physical_height = height
    elif native:
        host_dpr = scale_override or (host.scale if host.scale > 0 else 1.0)
        logical_w = width_override or host.logical_width or host.width
        logical_h = height_override or host.logical_height or host.height
        physical_w = host.physical_width or logical_w
        physical_h = host.physical_height or logical_h
        true_retina = (
            physical_w > 0
            and logical_w > 0
            and physical_w >= int(logical_w * 1.5)
        )
        if true_retina:
            width, height = physical_w, physical_h
            scale = scale_override or (host_dpr if host_dpr > 1.01 else float(physical_w) / float(logical_w))
        else:
            width, height = logical_w, logical_h
            scale = scale_override or 1.0
    else:
        width = width_override or 1280
        height = height_override or 800
        scale = scale_override or 1.0
        host_dpr = scale

    host_m = platform.machine().lower()
    host_is_arm = host_m in ("aarch64", "arm64", "armv8", "armv9")
    host_is_x86 = host_m in ("x86_64", "amd64", "x64")

    if arch == "aarch64":
        if host_os() == "darwin" and host_is_arm:
            accel = ["-accel", "hvf", "-cpu", "host"]
        elif is_linux() and host_is_arm:
            kvm = Path("/dev/kvm")
            if kvm.exists() and os.access(kvm, os.R_OK | os.W_OK):
                accel = ["-enable-kvm", "-cpu", "host"]
            else:
                accel = ["-cpu", "cortex-a72"]
        else:
            accel = ["-cpu", "cortex-a72"]
    else:
        if is_linux() and host_is_x86:
            kvm = Path("/dev/kvm")
            if kvm.exists() and os.access(kvm, os.R_OK | os.W_OK):
                accel = ["-enable-kvm", "-cpu", "host"]
            else:
                accel = ["-cpu", "max"]
        elif host_os() == "darwin" and host_is_x86:
            accel = ["-accel", "hvf", "-cpu", "host"]
        else:
            accel = ["-cpu", "max"]

    want_gl = os.environ.get("LCL_QEMU_GL", "").lower() in ("1", "true", "yes", "on")

    extra_qemu: list[str] = []

    usb_target = usb_passthrough or os.environ.get("LCL_QEMU_USB", "")
    if usb_target:
        if ":" in usb_target:
            parts = usb_target.split(":", 1)
            vid = parts[0].strip()
            pid = parts[1].strip()
            vid = vid if vid.startswith("0x") else f"0x{vid}"
            pid = pid if pid.startswith("0x") else f"0x{pid}"
            extra_qemu.extend([
                "-device", "qemu-xhci,id=xhci",
                "-device", f"usb-host,vendorid={vid},productid={pid}",
            ])
            log(f"USB Passthrough enabled for device {vid}:{pid}")
        else:
            log(f"WARNING: Invalid USB passthrough spec '{usb_target}'. Format must be VENDOR:PRODUCT (e.g. 046d:c077)")

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

    # Probe available devices once
    try:
        dev_help = subprocess.check_output(
            [qemu, "-device", "help"],
            text=True,
            stderr=subprocess.STDOUT,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        dev_help = ""
    has_apple_gfx = "apple-gfx-pci" in dev_help
    has_virtio_vga_gl = "virtio-vga-gl" in dev_help
    has_virtio_gpu_gl = "virtio-gpu-gl" in dev_help or "virtio-gpu-gl-pci" in dev_help
    has_virtio_vga = "virtio-vga" in dev_help

    if arch == "aarch64":
        # On aarch64 virt machine, set explicit resolution on virtio-gpu-pci to initialize FB immediately
        if want_gl and has_virtio_gpu_gl:
            gpu = ["-device", f"virtio-gpu-gl-pci,xres={width},yres={height}"]
        else:
            gpu = ["-device", f"virtio-gpu-pci,xres={width},yres={height}"]
    else:
        # x86_64
        if want_gl:
            if has_virtio_vga_gl:
                gpu = ["-vga", "none", "-device", "virtio-vga-gl"]
            else:
                log("virtio-vga-gl is not available on host QEMU (e.g. macOS); falling back to virtio-vga.")
                gpu = ["-vga", "none", "-device", "virtio-vga" if has_virtio_vga else f"virtio-gpu-pci,xres={width},yres={height}"]
        else:
            gpu = ["-vga", "none", "-device", "virtio-vga" if has_virtio_vga else f"virtio-gpu-pci,xres={width},yres={height}"]

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
    print(f"  Launching QEMU Virtual Machine ({arch}):")
    print(f"  - Architecture: {arch}")
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
    print(f"  - Boot Mode: {'ISO CD-ROM (' + ('UEFI' if uefi_mode else 'BIOS') + ')' if iso_mode else 'Direct Kernel Boot'}")
    print(f"  - Accelerator: {' '.join(accel)}")
    print(f"  - GPU: {' '.join(gpu)}{'  (LCL_QEMU_GL=1 for VirGL)' if not want_gl else ''}")
    print(f"  - Display: {display[1]}")
    if iso_mode:
        iso_name = f"lcl-os-{arch}.iso"
        iso_file = BUILD_DIR / iso_name if (BUILD_DIR / iso_name).is_file() else BUILD_DIR / "lcl-os.iso"
        print(f"  - ISO Image: {iso_file}")
    else:
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

    serial_console = (
        "console=ttyAMA0,115200 console=tty0 earlycon"
        if arch == "aarch64"
        else "console=tty0 console=ttyS0,115200 earlyprintk=ttyS0,115200"
    )
    append = (
        f"{serial_console} "
        f"{video_mode} {lcl_params} "
        f"rdinit=/init loglevel=6"
    )

    machine = ["-machine", "virt,highmem=on"] if arch == "aarch64" else ["-machine", "q35"]

    cmd = [
        qemu,
        *accel,
        *machine,
    ]

    ovmf = find_ovmf_firmware(arch)
    if uefi_mode:
        if not ovmf:
            err(f"UEFI requested (--uefi), but firmware file for {arch} not found.")
            sys.exit(1)
        log(f"UEFI boot mode enabled. Firmware: {ovmf}")
        if arch == "aarch64":
            cmd.extend(["-bios", str(ovmf)])
        else:
            cmd.extend(["-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}"])
    elif arch == "aarch64" and ovmf:
        # Attach EDK2 firmware on aarch64 to assist EFI kernel booting if available
        cmd.extend(["-bios", str(ovmf)])

    if iso_mode:
        iso_name = f"lcl-os-{arch}.iso"
        iso_path = BUILD_DIR / iso_name if (BUILD_DIR / iso_name).is_file() else BUILD_DIR / "lcl-os.iso"
        if not iso_path.is_file():
            err(f"Missing ISO file: {iso_path}. Please run 'make iso ARCH={arch}' first.")
            sys.exit(1)
        cmd.extend(["-boot", "d", "-cdrom", str(iso_path)])
    else:
        rootfs_img = BUILD_DIR / "rootfs" / f"lcl-rootfs-{arch}.ext4"
        if not rootfs_img.is_file():
            from build_rootfs import build_rootfs_ext4
            build_rootfs_ext4(arch=arch)
        cmd.extend([
            "-kernel",
            str(kernel),
            "-initrd",
            str(INITRAMFS_IMG),
            "-append",
            append,
            "-drive",
            f"file={rootfs_img},format=raw,if=virtio,id=rootfs",
        ])

    cmd.extend([
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
    ])
    log("QEMU cmdline: " + " ".join(cmd))
    os.execvp(qemu, cmd)


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux QEMU launcher")
    parser.add_argument("--run", "-r", action="store_true", help="Launch QEMU after packaging")
    parser.add_argument("--build-only", action="store_true", help="Only build binaries (Docker)")
    parser.add_argument("--package-only", action="store_true", help="Build + package initramfs (no QEMU)")
    parser.add_argument("--iso", action="store_true", help="Boot from build/lcl-os.iso CD-ROM image")
    parser.add_argument("--uefi", action="store_true", help="Use OVMF UEFI firmware for QEMU boot")
    parser.add_argument("--clean", action="store_true", help="Remove build/ directory safely")
    parser.add_argument(
        "--arch",
        "-a",
        metavar="ARCH",
        help="Target architecture: x86_64 (amd64) or aarch64 (arm64)",
    )
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
        "--retina",
        action="store_true",
        help="Launch in 13\" MacBook Air Retina mode (2560x1600 @ 2.0x UI scale)",
    )
    parser.add_argument(
        "--scale",
        type=float,
        metavar="FACTOR",
        help="Set custom UI scale factor (e.g. 1.25, 1.5, 2.0)",
    )
    parser.add_argument(
        "--width",
        type=int,
        metavar="PX",
        help="Set custom display width in pixels (e.g. 2560)",
    )
    parser.add_argument(
        "--height",
        type=int,
        metavar="PX",
        help="Set custom display height in pixels (e.g. 1600)",
    )
    parser.add_argument(
        "--gpu",
        "-g",
        action="store_true",
        help="Enable 3D VirGL GPU acceleration in QEMU",
    )
    parser.add_argument(
        "--trace-frames",
        action="store_true",
        help="Enable one-second client layout/render/resize trace in the guest",
    )
    parser.add_argument(
        "--debug-layout",
        action="store_true",
        help="Draw Yoga widget bounds and client damage rects in the guest",
    )
    parser.add_argument(
        "--debug-overlay",
        action="store_true",
        help="Show the compositor FPS and compose-time overlay in the guest",
    )
    parser.add_argument(
        "--usb",
        metavar="VENDOR:PRODUCT",
        help="Pass through host USB device to QEMU (e.g. --usb 046d:c077 or USB=046d:c077 with make qemu)",
    )
    parser.add_argument(
        "--inside-docker",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="Skip Docker build/package step; launch QEMU using existing cached kernel/initramfs artifacts.",
    )
    args = parser.parse_args()

    arch = normalize_arch(args.arch or os.environ.get("ARCH"))

    if args.gpu:
        os.environ["LCL_QEMU_GL"] = "1"

    if args.clean:
        clean_build(arch=arch)
        return

    print("====================================================")
    print(f"  LCL Core Linux - QEMU Isolated Boot Launcher ({arch})")
    print("====================================================")

    # ---- Inside Docker image: compile + package with image kernel/modules only ----
    if args.inside_docker:
        if args.build_only:
            cmake_build()
            log(f"Docker build-only ({arch}) complete.")
            return
        kernel = package_inside_docker(args)
        log(f"Packaging complete ({arch}). Kernel cache: {kernel}")
        return

    # ---- Outer host (Linux/macOS/Windows): always Docker for artifacts, QEMU on host ----
    # Host OS kernel/modules/userland are NEVER used for the guest initramfs.
    if args.build_only:
        build_only(arch=arch)
        return

    if args.no_build:
        # Skip Docker build/package; use whatever is already cached.
        if not KERNEL_CACHE.is_file() or not INITRAMFS_IMG.is_file():
            err(
                "--no-build requires pre-built artifacts but none were found.\n"
                f"  Expected kernel:    {KERNEL_CACHE}\n"
                f"  Expected initramfs: {INITRAMFS_IMG}\n"
                "Run without --no-build first to build them."
            )
            sys.exit(1)
        kernel = KERNEL_CACHE
        log(f"[--no-build] Skipping Docker build. Using cached kernel: {kernel}")
    else:
        kernel = prepare_artifacts(args, arch=arch)

    if args.package_only:
        log(f"Packaging complete ({arch}). Kernel cache: {kernel}")
        return

    if args.run:
        log(f"QEMU binary: {find_qemu(arch)}")
        log(f"Kernel: {kernel}")
        launch_qemu(
            kernel,
            arch=arch,
            native=args.native,
            iso_mode=args.iso,
            uefi_mode=args.uefi,
            usb_passthrough=args.usb,
            retina=args.retina,
            scale_override=args.scale,
            width_override=args.width,
            height_override=args.height,
        )
    else:
        log(f"Boot environment ({arch}) ready! Kernel: {kernel}")
        log(f"Run '{Path(sys.argv[0]).name} --run --arch {arch}' to launch QEMU in live VM.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        err(f"Command failed with exit code {exc.returncode}: {exc.cmd}")
        sys.exit(exc.returncode or 1)
    except KeyboardInterrupt:
        sys.exit(130)
