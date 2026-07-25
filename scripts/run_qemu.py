#!/usr/bin/env python3
"""LCL QEMU launcher — Linux native; macOS/Windows via Docker."""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
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


def launch_qemu(kernel: Path) -> None:
    qemu = find_qemu()
    if not INITRAMFS_IMG.is_file():
        err(f"Missing initramfs: {INITRAMFS_IMG}")
        sys.exit(1)
    if not kernel.is_file():
        err(f"Missing kernel: {kernel}")
        sys.exit(1)

    memory = "2G"
    cpus = "2"
    kvm = Path("/dev/kvm")
    if kvm.exists() and os.access(kvm, os.R_OK | os.W_OK):
        accel = ["-enable-kvm", "-cpu", "host"]
    else:
        accel = ["-cpu", "max"]

    # Prefer Cocoa on macOS, SDL elsewhere when available
    display = ["-display", "cocoa"] if host_os() == "darwin" else ["-display", "sdl,gl=on"]
    gpu = ["-device", "virtio-vga-gl"]
    # Cocoa + gl can fail on some setups; fall back without gl device flags if needed
    if host_os() == "darwin":
        gpu = ["-device", "virtio-vga"]
        display = ["-display", "cocoa"]

    print("----------------------------------------------------")
    print("  Launching QEMU Virtual Machine:")
    print(f"  - Memory: {memory}")
    print(f"  - SMP Cores: {cpus}")
    print(f"  - Accelerator: {' '.join(accel)}")
    print(f"  - GPU: {gpu[1]}")
    print(f"  - Display: {display[1]}")
    print("----------------------------------------------------")

    cmd = [
        qemu,
        *accel,
        "-kernel",
        str(kernel),
        "-initrd",
        str(INITRAMFS_IMG),
        "-append",
        "console=tty0 console=ttyS0,115200 video=1280x800-32@144 earlyprintk=ttyS0 rdinit=/init quiet loglevel=3",
        "-m",
        memory,
        "-smp",
        cpus,
        *gpu,
        *display,
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
            launch_qemu(kernel if isinstance(kernel, Path) else Path(kernel))
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
        launch_qemu(kernel)
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
