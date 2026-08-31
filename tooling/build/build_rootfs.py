#!/usr/bin/env python3
"""
LCL Core Linux - Canonical RootFS Artifact Builder (Stage 4C.2d)
Canonical LCL Filesystem & Application ABI v1

Builds a single, deterministic, platform-independent ext4 root filesystem artifact:
    out/rootfs/lcl-rootfs-<arch>.ext4
from the canonical staging tree:
    out/rootfs/<arch>/

Canonical Root Namespace:
    /Applications/              # Machine-wide installed applications
    /Library/                   # Machine-wide mutable resources & config
    /Runtime/                   # Boot & session runtime state (/Runtime/Sessions/Rei, /Runtime/Temporary)
    /System/                    # Immutable OS content (Applications, Core, Tools, Library)
        /System/Applications/   # Built-in .app bundles (Terminal.app)
        /System/Core/           # LCL daemons/tools and both Gestalt-selectable shells
        /System/Tools/          # Core utilities & Bash (/System/Tools/bash, coreutils)
        /System/Library/        # Fonts, Wallpapers, Libraries (Mesa drivers, glibc, etc.)
    /Users/                     # User home directories (/Users/Rei, /Users/Shared)
    /Volumes/                   # Mounted volumes and external filesystems

Linux Kernel & Runtime Compatibility (Implementation Details):
    /dev, /proc, /sys           (kernel virtual filesystems)
    /lib64, /lib, /usr/lib      (compatibility symlinks -> /System/Library/Libraries)
    /bin, /usr/bin              (compatibility symlinks -> /System/Core and /System/Tools)
    /tmp -> /Runtime/Temporary
    /run -> /Runtime
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.build.skia_package import host_skia_cmake_args
from tooling.paths import OUT_ROOT, QEMU_CACHE_DIR, ROOTFS_DIR, qemu_build_dir

BUILD_DIR = OUT_ROOT
ROOTFS_BASE_DIR = ROOTFS_DIR
DOCKERFILE = SCRIPT_DIR / "Dockerfile.qemu"
ROOTFS_FINGERPRINT_VERSION = 1

CANONICAL_TARGETS = (
    "lcl-desktop-shell",
    "lcl-mobile-shell",
    "lcl-shell-launcher",
    "lcl-terminal",
    "lcl-sessiond",
    "lcl-sandboxd",
    "lcl-securityd",
    "lcl-sandbox-probe",
    "lcl-sandbox-smoke",
    "lcl-rasterd",
    "lcl-open",
    "lcl-core",
    "lcl-js",
)

ARCH_META: dict[str, dict] = {
    "x86_64": {
        "triplet": "x86_64-linux-gnu",
        "loader": "ld-linux-x86-64.so.2",
        "loader_candidates": [
            "/lib64/ld-linux-x86-64.so.2",
            "/usr/lib64/ld-linux-x86-64.so.2",
            "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
            "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
            "/usr/lib/ld-linux-x86-64.so.2",
        ],
        "docker_platform": "linux/amd64",
    },
    "aarch64": {
        "triplet": "aarch64-linux-gnu",
        "loader": "ld-linux-aarch64.so.1",
        "loader_candidates": [
            "/lib/ld-linux-aarch64.so.1",
            "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
            "/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
            "/usr/lib/ld-linux-aarch64.so.1",
        ],
        "docker_platform": "linux/arm64",
    },
}


def log(msg: str) -> None:
    print(f"[LCL ROOTFS] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL ROOTFS ERROR] {msg}", file=sys.stderr, flush=True)


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


def get_sha256(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def find_host_bin(name: str) -> Path | None:
    w = shutil.which(name)
    if w and Path(w).is_file():
        return Path(w)
    for cand in (f"/usr/bin/{name}", f"/bin/{name}", f"/usr/sbin/{name}", f"/sbin/{name}"):
        cp = Path(cand)
        if cp.is_file():
            return cp
    return None


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
            target = dest_lib / Path(lib_path).name
            if not target.exists():
                shutil.copy2(lib_path, target, follow_symlinks=True)


def is_binary_matching_arch(binary_path: Path | None, arch: str) -> bool:
    if not binary_path or not binary_path.is_file():
        return False
    try:
        with open(binary_path, "rb") as f:
            header = f.read(20)
            if len(header) < 20 or header[:4] != b"\x7fELF":
                return False
            # e_machine is at offset 18 (2 bytes little-endian)
            import struct
            e_machine = struct.unpack("<H", header[18:20])[0]
            norm_arch = normalize_arch(arch)
            if norm_arch == "aarch64":
                return e_machine == 183  # EM_AARCH64 (0xB7)
            elif norm_arch == "x86_64":
                return e_machine == 62   # EM_X86_64 (0x3E)
    except Exception:
        pass
    return False


def canonical_build_dir(arch: str) -> Path:
    # A dedicated QEMU tree permits Ninja without deleting a developer-owned
    # Unix Makefiles cache and keeps test builds out of the packaged binaries.
    return qemu_build_dir(normalize_arch(arch))


def find_built_binary(name: str, arch: str | None = None) -> Path | None:
    roots = [canonical_build_dir(arch)] if arch is not None else []
    for root in roots:
        for cand in [
            root / name,
            root / "apps" / name / name,
        ]:
            if not cand.is_file():
                continue
            if arch is not None and not is_binary_matching_arch(cand, arch):
                continue
            return cand
    return None


def ensure_binaries(arch: str = "x86_64") -> dict[str, Path]:
    """Ensures that required canonical LCL application and tool binaries exist for the target architecture."""
    norm_arch = normalize_arch(arch)

    # Existence alone is not freshness. Always let CMake's dependency graph
    # update these targets so a protocol/header change cannot be packaged with
    # stale canonical userspace binaries.
    target_build_dir = canonical_build_dir(norm_arch)
    try:
        skia_args = host_skia_cmake_args(PROJECT_ROOT, norm_arch)
    except RuntimeError:
        package_target = f"host-{norm_arch}"
        log(f"Preparing missing Skia package {package_target} inside target Docker...")
        subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "prepare_skia.py"),
             "--target", package_target],
            check=True,
        )
        skia_args = host_skia_cmake_args(PROJECT_ROOT, norm_arch)
    cache_exists = (target_build_dir / "CMakeCache.txt").is_file()
    configure = [
        "cmake", "-B", str(target_build_dir), "-S", str(PROJECT_ROOT),
        "-DBUILD_TESTS=OFF",
        *skia_args,
    ]
    if not cache_exists:
        if shutil.which("ninja"):
            configure.extend(["-G", "Ninja"])
        if shutil.which("ccache"):
            configure.extend([
                "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
            ])
    subprocess.run(configure, check=True)
    log(f"Updating canonical targets for {norm_arch}...")
    subprocess.run(
        ["cmake", "--build", str(target_build_dir),
         "--parallel", str(os.cpu_count() or 4), "--target"] +
        list(CANONICAL_TARGETS),
        check=True,
    )

    binaries: dict[str, Path] = {}
    for name in CANONICAL_TARGETS:
        p = find_built_binary(name, norm_arch)
        if not p or not p.is_file():
            raise RuntimeError(f"Required canonical binary '{name}' was not found or is not {norm_arch} after build.")
        binaries[name] = p

    return binaries


def _hash_input_path(digest, path: Path) -> None:
    """Add one project-controlled rootfs input to a stable content digest."""
    relative = path.relative_to(PROJECT_ROOT).as_posix()
    digest.update(relative.encode("utf-8"))
    digest.update(b"\0")
    if path.is_symlink():
        digest.update(b"link\0")
        digest.update(os.readlink(path).encode("utf-8"))
        digest.update(b"\0")
        return
    digest.update(b"file\0")
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    digest.update(b"\0")


def rootfs_input_fingerprint(arch: str, binaries: dict[str, Path]) -> str:
    """Hash every project-controlled input embedded in the canonical rootfs."""
    norm_arch = normalize_arch(arch)
    digest = hashlib.sha256()
    digest.update(f"lcl-rootfs-v{ROOTFS_FINGERPRINT_VERSION}\0{norm_arch}\0".encode("utf-8"))
    digest.update(os.environ.get("LCL_QEMU_BUILDER_IMAGE_ID", "unmanaged-host").encode("utf-8"))
    digest.update(b"\0")

    source_inputs = (
        Path(__file__).resolve(),
        PROJECT_ROOT / "assets" / "fonts",
        PROJECT_ROOT / "assets" / "wallpapers" / "default.jpg",
        # mobile.json is intentionally absent: QEMU transports that profile at
        # launch time through fw_cfg, so editing it never invalidates rootfs.
        PROJECT_ROOT / "config" / "gestalt" / "default.json",
        PROJECT_ROOT / "apps" / "terminal" / "Manifest.json",
        PROJECT_ROOT / "apps" / "terminal" / "Resources" / "Icon.png",
        PROJECT_ROOT / "apps" / "sandbox_probe" / "Manifest.json",
    )
    for source_input in source_inputs:
        if source_input.is_dir():
            for child in sorted(source_input.rglob("*")):
                if child.is_file() or child.is_symlink():
                    _hash_input_path(digest, child)
        elif source_input.is_file() or source_input.is_symlink():
            _hash_input_path(digest, source_input)
        else:
            missing = source_input.relative_to(PROJECT_ROOT).as_posix()
            digest.update(f"missing:{missing}\0".encode("utf-8"))

    for name in sorted(binaries):
        digest.update(f"binary:{name}\0".encode("utf-8"))
        digest.update(get_sha256(binaries[name]).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def rootfs_fingerprint_path(arch: str) -> Path:
    return QEMU_CACHE_DIR / f"rootfs-{normalize_arch(arch)}.sha256"


def docker_image_fingerprint_path(arch: str) -> Path:
    return QEMU_CACHE_DIR / f"docker-image-{normalize_arch(arch)}.stamp"


def stage_canonical_rootfs(
    staging_dir: Path,
    arch: str = "x86_64",
    binaries: dict[str, Path] | None = None,
) -> dict[str, str]:
    """Populates the deterministic canonical userspace filesystem layout."""
    norm_arch = normalize_arch(arch)
    meta = ARCH_META[norm_arch]
    triplet = meta["triplet"]
    loader_name = meta["loader"]

    log(f"Staging canonical LCL userspace at {staging_dir} ({norm_arch}, triplet={triplet})...")
    if staging_dir.exists():
        shutil.rmtree(staging_dir)

    # 1. Canonical Root Directory Hierarchy
    canonical_dirs = [
        "Applications",
        "Library",
        "Runtime/Sessions/Rei",
        "Runtime/Temporary",
        "System/Applications",
        "System/Core",
        "System/Tools",
        "System/Library/Fonts",
        "System/Library/Gestalt",
        "System/Library/Wallpapers",
        "System/Library/Libraries",
        "Users/Rei/Applications",
        "Users/Rei/Desktop",
        "Users/Rei/Documents",
        "Users/Rei/Downloads",
        "Users/Rei/Library/Containers/org.lcl.terminal/Data",
        "Users/Rei/Library/Containers/org.lcl.terminal/Cache",
        "Users/Rei/Library/Containers/org.lcl.terminal/Preferences",
        "Users/Rei/Library/Containers/org.lcl.terminal/Temporary",
        "Users/Rei/Movies",
        "Users/Rei/Music",
        "Users/Rei/Pictures",
        "Users/Shared",
        "Volumes",
        # Kernel & Compatibility Mountpoints
        "proc",
        "sys",
        "dev",
        "etc",
        "var/log",
        "var/lib/lcl-security",
        "usr/share",
    ]
    for sub in canonical_dirs:
        (staging_dir / sub).mkdir(parents=True, exist_ok=True)

    dest_system_core = staging_dir / "System" / "Core"
    dest_system_tools = staging_dir / "System" / "Tools"
    dest_system_lib = staging_dir / "System" / "Library" / "Libraries"
    dest_system_fonts = staging_dir / "System" / "Library" / "Fonts"
    dest_system_gestalt = staging_dir / "System" / "Library" / "Gestalt"
    dest_system_wallpapers = staging_dir / "System" / "Library" / "Wallpapers"
    dest_system_apps = staging_dir / "System" / "Applications"

    # 2. Linux Kernel & Compatibility Symlinks
    compat_symlinks = [
        ("lib64", "System/Library/Libraries"),
        ("lib", "System/Library/Libraries"),
        ("bin", "System/Core"),
        ("sbin", "System/Tools"),
        ("tmp", "Runtime/Temporary"),
        ("run", "Runtime"),
        ("home/user", "../Users/Rei"),
    ]
    for link_rel, target in compat_symlinks:
        link_path = staging_dir / link_rel
        if link_path.parent != staging_dir:
            link_path.parent.mkdir(parents=True, exist_ok=True)
        if not link_path.exists() and not link_path.is_symlink():
            link_path.symlink_to(target)

    # /usr compatibility links
    usr_dir = staging_dir / "usr"
    (usr_dir / "lib").symlink_to("../System/Library/Libraries")
    (usr_dir / "bin").symlink_to("../System/Core")
    (usr_dir / "share" / "fonts").symlink_to("../../System/Library/Fonts")
    (usr_dir / "share" / "wallpapers").symlink_to("../../System/Library/Wallpapers")

    # 3. Dynamic Linker (Arch-specific)
    loader_found = False
    for loader_cand in meta["loader_candidates"]:
        cand_p = Path(loader_cand)
        if cand_p.is_file():
            shutil.copy2(cand_p.resolve(), dest_system_lib / loader_name)
            loader_found = True
            log(f"  ✓ Found target dynamic linker: {cand_p} -> {loader_name}")
            break
    if not loader_found:
        raise RuntimeError(f"Dynamic linker {loader_name} not found for architecture {norm_arch}.")

    # Also make sure standard /lib/ld-linux-* or /lib64/ld-linux-* link resolves
    if norm_arch == "aarch64":
        lib_ld = staging_dir / "lib" / loader_name
        if not lib_ld.exists() and not lib_ld.is_symlink():
            lib_ld.symlink_to(f"../System/Library/Libraries/{loader_name}")
    elif norm_arch == "x86_64":
        lib64_ld = staging_dir / "lib64" / loader_name
        if not lib64_ld.exists() and not lib64_ld.is_symlink():
            lib64_ld.symlink_to(f"../System/Library/Libraries/{loader_name}")

    # 4. Canonical LCL Core Daemons & Tools
    binaries = binaries or ensure_binaries(norm_arch)
    sha_map: dict[str, str] = {}
    core_daemons = {
        "lcl-desktop-shell": dest_system_core / "lcl-desktop-shell",
        "lcl-mobile-shell": dest_system_core / "lcl-mobile-shell",
        "lcl-shell-launcher": dest_system_core / "lcl-shell-launcher",
        "lcl-sessiond": dest_system_core / "lcl-sessiond",
        "lcl-sandboxd": dest_system_core / "lcl-sandboxd",
        "lcl-securityd": dest_system_core / "lcl-securityd",
        "lcl-sandbox-probe": dest_system_core / "lcl-sandbox-probe",
        "lcl-rasterd": dest_system_core / "lcl-rasterd",
        "lcl-open": dest_system_core / "lcl-open",
        "lcl-core": dest_system_core / "lcl-core",
        "lcl-js": dest_system_core / "lcl-js",
    }
    for name, dst in core_daemons.items():
        src = binaries[name]
        shutil.copy2(src, dst)
        dst.chmod(0o755)
        sha_map[name] = get_sha256(dst)
        copy_ldd_deps(dst, dest_system_lib)

    # Standard open symlink in System/Core
    open_sym = dest_system_core / "open"
    if open_sym.exists() or open_sym.is_symlink():
        open_sym.unlink()
    open_sym.symlink_to("lcl-open")

    # 5. GNU Bash and System Tools (/System/Tools/)
    bash_src = find_host_bin("bash") or find_host_bin("sh")
    if not bash_src:
        raise RuntimeError("Target bash binary not found.")
    bash_dst = dest_system_tools / "bash"
    shutil.copy2(bash_src, bash_dst)
    bash_dst.chmod(0o755)
    sha_map["bash"] = get_sha256(bash_dst)
    copy_ldd_deps(bash_dst, dest_system_lib)

    # Symlink /System/Tools/sh -> bash
    (dest_system_tools / "sh").symlink_to("bash")

    # Essential tools
    util_names = [
        "mount", "mkdir", "sleep", "ls", "cat", "id", "uname", "grep",
        "printf", "dmesg", "tee", "find", "cp", "mv", "rm", "chmod",
        "chown", "touch", "wc", "head", "tail", "clear"
    ]
    for u in util_names:
        u_src = find_host_bin(u)
        if u_src and u_src.is_file():
            u_dst = dest_system_tools / u
            shutil.copy2(u_src, u_dst)
            u_dst.chmod(0o755)
            copy_ldd_deps(u_dst, dest_system_lib)

    # The copied ncurses clear binary consults the terminfo database using the
    # TERM value established for every Terminal.app PTY.  Keep the matching
    # entry in the canonical rootfs rather than shipping a clear binary whose
    # terminal contract cannot be fulfilled.
    terminfo_name = "xterm-256color"
    terminfo_source = next((
        root / terminfo_name[0] / terminfo_name
        for root in (Path("/usr/share/terminfo"), Path("/lib/terminfo"), Path("/etc/terminfo"))
        if (root / terminfo_name[0] / terminfo_name).is_file()
    ), None)
    if terminfo_source is None:
        raise RuntimeError(f"Required terminfo entry '{terminfo_name}' was not found.")
    terminfo_destination = staging_dir / "usr" / "share" / "terminfo" / terminfo_name[0]
    terminfo_destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(terminfo_source, terminfo_destination / terminfo_name)

    # Fallback clear script
    if not (dest_system_tools / "clear").is_file():
        (dest_system_tools / "clear").write_text("#!/bin/sh\nprintf \"\\033[2J\\033[H\"\n")
        (dest_system_tools / "clear").chmod(0o755)

    # 6. Graphics & EGL Drivers (Mesa DRI, GBM, VirGL, libinput)
    dri_dirs = [
        Path(f"/usr/lib/{triplet}/dri"),
        Path("/usr/lib/dri"),
        Path("/usr/lib64/dri"),
    ]
    dri_dst = dest_system_lib / "dri"
    dri_dst.mkdir(parents=True, exist_ok=True)
    for dri_src in dri_dirs:
        if dri_src.is_dir():
            for dri_item in dri_src.iterdir():
                if dri_item.is_file() or dri_item.is_symlink():
                    if dri_item.is_symlink():
                        target_name = os.readlink(str(dri_item))
                        (dri_dst / dri_item.name).unlink(missing_ok=True)
                        try:
                            os.symlink(target_name, str(dri_dst / dri_item.name))
                        except OSError:
                            pass
                    else:
                        shutil.copy2(dri_item, dri_dst / dri_item.name)
                        copy_ldd_deps(dri_item, dest_system_lib)
            break

    gbm_dirs = [
        Path(f"/usr/lib/{triplet}/gbm"),
        Path("/usr/lib/gbm"),
        Path("/usr/lib64/gbm"),
    ]
    gbm_dst = dest_system_lib / "gbm"
    gbm_dst.mkdir(parents=True, exist_ok=True)
    for gbm_src in gbm_dirs:
        if gbm_src.is_dir():
            for gbm_item in gbm_src.iterdir():
                if gbm_item.is_file() or gbm_item.is_symlink():
                    if gbm_item.is_symlink():
                        target_name = os.readlink(str(gbm_item))
                        (gbm_dst / gbm_item.name).unlink(missing_ok=True)
                        try:
                            os.symlink(target_name, str(gbm_dst / gbm_item.name))
                        except OSError:
                            pass
                    else:
                        shutil.copy2(gbm_item, gbm_dst / gbm_item.name)
                        copy_ldd_deps(gbm_item, dest_system_lib)
            break

    # Copy Mesa backend libraries into dest_system_lib
    for mesa_cand in [Path(f"/usr/lib/{triplet}"), Path("/usr/lib64"), Path("/usr/lib")]:
        if mesa_cand.is_dir():
            for pat in ("libEGL*", "libGL*", "libgbm*", "libglapi*", "libdrm*", "libgallium*", "libLLVM*"):
                for m_so in mesa_cand.glob(pat):
                    if m_so.is_file():
                        dst = dest_system_lib / m_so.name
                        if not dst.exists():
                            try:
                                shutil.copy2(m_so.resolve(), dst)
                                copy_ldd_deps(dst, dest_system_lib)
                            except Exception:
                                pass

    # Symlink target triplet -> . inside dest_system_lib
    triplet_link = dest_system_lib / triplet
    if triplet_link.exists() or triplet_link.is_symlink():
        if triplet_link.is_dir() and not triplet_link.is_symlink():
            shutil.rmtree(triplet_link)
        else:
            triplet_link.unlink()
    triplet_link.symlink_to(".")

    # 7. EGL / GLVND Vendor Discovery & DRI Runtime (/System/Library/EGL/)
    dest_system_egl = staging_dir / "System" / "Library" / "EGL"
    dest_system_glvnd = dest_system_egl / "glvnd"
    dest_system_egl_vendor = dest_system_glvnd / "egl_vendor.d"
    dest_system_egl_vendor.mkdir(parents=True, exist_ok=True)

    glvnd_src = Path("/usr/share/glvnd")
    if glvnd_src.is_dir():
        shutil.copytree(glvnd_src, dest_system_glvnd, dirs_exist_ok=True)

    mesa_json_path = dest_system_egl_vendor / "50_mesa.json"
    if not mesa_json_path.is_file():
        mesa_json_path.write_text(json.dumps({
            "file_format_version": "1.0.0",
            "ICD": {
                "library_path": "libEGL_mesa.so.0"
            }
        }, indent=4), encoding="utf-8")

    drirc_src = Path("/usr/share/drirc.d")
    if drirc_src.is_dir():
        shutil.copytree(drirc_src, dest_system_egl / "drirc.d", dirs_exist_ok=True)

    # 8. libinput Device Quirks & Hardware Data (/System/Library/Input/libinput)
    dest_system_input = staging_dir / "System" / "Library" / "Input" / "libinput"
    dest_system_input.mkdir(parents=True, exist_ok=True)
    libinput_src = Path("/usr/share/libinput")
    if libinput_src.is_dir():
        shutil.copytree(libinput_src, dest_system_input, dirs_exist_ok=True)

    # Compatibility symlinks in /usr/share and /etc for third-party libraries (GLVND & libinput)
    (usr_dir / "share" / "glvnd").symlink_to("../../System/Library/EGL/glvnd")
    (usr_dir / "share" / "libinput").symlink_to("../../System/Library/Input/libinput")
    if (dest_system_egl / "drirc.d").is_dir():
        (usr_dir / "share" / "drirc.d").symlink_to("../../System/Library/EGL/drirc.d")
    (staging_dir / "etc" / "glvnd").symlink_to("../System/Library/EGL/glvnd")
    (staging_dir / "etc" / "libinput").symlink_to("../System/Library/Input/libinput")

    # 9. System Fonts (/System/Library/Fonts/)
    fonts_src = PROJECT_ROOT / "assets" / "fonts"
    if fonts_src.is_dir():
        for fam in ["jetbrains-mono", "inter", "liberation-sans", "liberation-serif"]:
            src_fam = fonts_src / fam
            if src_fam.is_dir():
                shutil.copytree(src_fam, dest_system_fonts / fam, dirs_exist_ok=True)

    # 10. Wallpapers (/System/Library/Wallpapers/)
    default_wallpaper = PROJECT_ROOT / "assets" / "wallpapers" / "default.jpg"
    if default_wallpaper.is_file():
        shutil.copy2(default_wallpaper, dest_system_wallpapers / "wallpaper.jpg")

    gestalt_src = PROJECT_ROOT / "config" / "gestalt" / "default.json"
    if not gestalt_src.is_file():
        raise RuntimeError(f"Missing default Gestalt: {gestalt_src}")
    shutil.copy2(gestalt_src, dest_system_gestalt / "default.json")

    # 11. Built-in App Bundles ABI v1 (/System/Applications/)
    # Terminal.app
    term_dst = dest_system_apps / "Terminal.app"
    term_dst.mkdir(parents=True, exist_ok=True)
    (term_dst / "Executables").mkdir(parents=True, exist_ok=True)
    (term_dst / "Resources").mkdir(parents=True, exist_ok=True)

    term_manifest = PROJECT_ROOT / "apps" / "terminal" / "Manifest.json"
    term_icon = PROJECT_ROOT / "apps" / "terminal" / "Resources" / "Icon.png"
    if not term_manifest.is_file() or not term_icon.is_file():
        raise RuntimeError(f"Missing Terminal.app source files in {PROJECT_ROOT / 'apps' / 'terminal'}")
    shutil.copy2(term_manifest, term_dst / "Manifest.json")
    shutil.copy2(term_icon, term_dst / "Resources" / "Icon.png")

    term_bin = binaries["lcl-terminal"]
    shutil.copy2(term_bin, term_dst / "Executables" / "Terminal")
    (term_dst / "Executables" / "Terminal").chmod(0o755)
    sha_map["Terminal.app"] = get_sha256(term_dst / "Executables" / "Terminal")
    copy_ldd_deps(term_dst / "Executables" / "Terminal", dest_system_lib)

    # Sandbox Probe.app is a non-GUI acceptance helper.  Unlike Terminal it
    # deliberately follows the ordinary third-party sandbox profile, so a
    # QEMU run can validate UID, namespaces, Landlock, cgroup setup and the
    # no-new-privileges boundary without exposing a compositor endpoint yet.
    smoke_dst = dest_system_apps / "Sandbox Probe.app"
    smoke_dst.mkdir(parents=True, exist_ok=True)
    (smoke_dst / "Executables").mkdir(parents=True, exist_ok=True)
    (smoke_dst / "Resources").mkdir(parents=True, exist_ok=True)
    smoke_manifest = PROJECT_ROOT / "apps" / "sandbox_probe" / "Manifest.json"
    if not smoke_manifest.is_file() or not term_icon.is_file():
        raise RuntimeError("Missing Sandbox Probe.app source files")
    shutil.copy2(smoke_manifest, smoke_dst / "Manifest.json")
    shutil.copy2(term_icon, smoke_dst / "Resources" / "Icon.png")
    smoke_bin = binaries["lcl-sandbox-smoke"]
    shutil.copy2(smoke_bin, smoke_dst / "Executables" / "SandboxProbe")
    (smoke_dst / "Executables" / "SandboxProbe").chmod(0o755)
    sha_map["Sandbox Probe.app"] = get_sha256(smoke_dst / "Executables" / "SandboxProbe")
    copy_ldd_deps(smoke_dst / "Executables" / "SandboxProbe", dest_system_lib)

    # Validate all app bundles
    validate_app_bundles(staging_dir)

    # 10. Canonical Environment & User Profiles
    (staging_dir / "etc" / "profile").write_text(
        "export PATH=/System/Core:/System/Tools:$PATH\n"
        "export HOME=/Users/Rei\n"
        "export USER=Rei\n"
        "export TERM=xterm-256color\n"
        "export HISTSIZE=500\n"
        "export HISTFILESIZE=1000\n"
        "alias ls='ls --color=auto'\n"
        "alias ll='ls -la'\n"
        "if [ -f /Users/Rei/.bashrc ]; then\n"
        "    . /Users/Rei/.bashrc\n"
        "fi\n"
    )

    (staging_dir / "etc" / "passwd").write_text(
        "root:x:0:0:root:/Users/Rei:/System/Tools/bash\n"
        "Rei:x:1000:1000:Rei:/Users/Rei:/System/Tools/bash\n"
    )
    (staging_dir / "etc" / "group").write_text(
        "root:x:0:\n"
        "Rei:x:1000:\n"
    )

    (staging_dir / "Users" / "Rei" / ".bashrc").write_text(
        "export PATH=/System/Core:/System/Tools:$PATH\n"
        "export HOME=/Users/Rei\n"
        "export USER=Rei\n"
        "export TERM=xterm-256color\n"
        "export PS1='\\[\\033[1;34m\\]\\W\\[\\033[0m\\] ❯ '\n"
        "export HISTSIZE=500\n"
        "alias ls='ls --color=auto'\n"
        "alias ll='ls -la'\n"
        "bind 'set completion-ignore-case on' 2>/dev/null || true\n"
        "bind 'set show-all-if-ambiguous on' 2>/dev/null || true\n"
        "bind 'TAB:menu-complete' 2>/dev/null || true\n"
        "bind '\"\\e[Z\":menu-complete-backward' 2>/dev/null || true\n"
    )

    (staging_dir / "Users" / "Rei" / ".profile").write_text(". /Users/Rei/.bashrc\n")

    # 11. Canonical RootFS /init Entrypoint
    init_script_content = """#!/System/Tools/sh
export PATH=/System/Core:/System/Tools
export HOME=/Users/Rei
export USER=Rei
export TERM=xterm-256color

# 1. Mount virtual kernel filesystems
mkdir -p /proc /sys /dev /Runtime /Runtime/Temporary /Runtime/Sessions/Rei /var/log /var/tmp
mount -t proc proc /proc -o nosuid,noexec,nodev 2>/dev/null || true
mount -t sysfs sysfs /sys -o nosuid,noexec,nodev 2>/dev/null || true
# A unified cgroup hierarchy is required before sandboxd can apply per-app
# memory, process-count and CPU limits. Keep booting when the kernel omits it;
# the native probe below will then report third-party sandboxing as unavailable.
mkdir -p /sys/fs/cgroup
mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev -o nosuid 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /dev/input
mount -t devpts devpts /dev/pts -o mode=0620,ptmxmode=0666 2>/dev/null || mount -t devpts devpts /dev/pts 2>/dev/null || true
if [ ! -e /dev/ptmx ]; then
    mknod -m 666 /dev/ptmx c 5 2 2>/dev/null || ln -sf pts/ptmx /dev/ptmx 2>/dev/null || true
fi
chmod 666 /dev/ptmx 2>/dev/null || true
mount -t tmpfs tmpfs /dev/shm -o mode=1777,nosuid,nodev 2>/dev/null || true
mount -t tmpfs tmpfs /Runtime -o mode=0755,nosuid,nodev 2>/dev/null || true
mkdir -p /Runtime/Sessions/Rei /Runtime/Temporary
chmod 0700 /Runtime/Sessions/Rei
chmod 1777 /Runtime/Temporary

# Setup compatibility mountpoints
mkdir -p /run /tmp
mount --bind /Runtime /run 2>/dev/null || true
mount --bind /Runtime/Temporary /tmp 2>/dev/null || true

# Wait for /dev/dri/card* (up to ~3s)
i=0
while [ "$i" -lt 30 ]; do
    if ls /dev/dri/card* >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

# Save kernel dmesg log and input inventory
dmesg > /var/log/dmesg.log 2>&1 || true
ls -la /dev/input/ > /var/log/input_devices.log 2>&1 || true

ROOT_DEV="unknown"
while read -r rdev rmp rtype rrest; do
    if [ "$rmp" = "/" ]; then
        ROOT_DEV="$rdev ($rtype)"
        break
    fi
done < /proc/mounts

echo "===================================================="
echo "  LCL Core Linux (LCL) - Canonical RootFS Session   "
echo "===================================================="
echo "Root device: $ROOT_DEV"
echo "Kernel: $(uname -r)  cmdline: $(cat /proc/cmdline 2>/dev/null)"
echo "User: Rei  Home: /Users/Rei"
echo "DRM devices detected:"
ls -la /dev/dri/ 2>/dev/null || echo "  (none)"
echo "Input devices detected:"
ls /dev/input/ 2>/dev/null || echo "  (none yet)"

# Probe as root before launching unprivileged session processes. The probe
# performs each namespace/seccomp check in its own short-lived child and
# writes the same report to the serial log and the persistent boot log.
if [ -x /System/Core/lcl-sandbox-probe ]; then
    echo "Sandbox kernel capabilities:"
    /System/Core/lcl-sandbox-probe 2>&1 | tee /var/log/lcl_sandbox_probe.log
else
    echo "[init] lcl-sandbox-probe is unavailable" | tee /var/log/lcl_sandbox_probe.log
fi

# Start Compositor Display Server
/System/Core/lcl-core 2>&1 | tee /var/log/lcl_compositor.log &
sleep 0.2

# securityd owns only the session user's unsigned-bundle allow/revoke
# decisions. It is deliberately separate from sandboxd's launch authority.
if [ -x /System/Core/lcl-securityd ]; then
    echo "[init] Starting lcl-securityd..."
    /System/Core/lcl-securityd --session-uid 1000 --session-gid 1000 \
        2>&1 | tee /var/log/lcl_securityd.log &
    sleep 0.1
fi

# Start LCL Session Daemon (sole application launch authority)
if [ -x /System/Core/lcl-sessiond ]; then
    echo "[init] Starting lcl-sessiond..."
    /System/Core/lcl-sessiond 2>&1 | tee /var/log/lcl_sessiond.log &
    sleep 0.1
fi

# sandboxd is a separate root-owned process.  Its owner-only socket is for
# lcl-sessiond, not for the interactive Terminal or normal application UIDs.
if [ -x /System/Core/lcl-sandboxd ]; then
    echo "[init] Starting lcl-sandboxd..."
    /System/Core/lcl-sandboxd --session-uid 0 --session-gid 0 --platform linux-full \
        2>&1 | tee /var/log/lcl_sandboxd.log &
    sleep 0.1
fi

# Start the one shell selected by the canonical Gestalt profile.
if [ -x /System/Core/lcl-shell-launcher ]; then
    echo "[init] Starting Gestalt-selected LCL shell..."
    /System/Core/lcl-shell-launcher 2>&1 | tee /var/log/lcl_shell.log &
fi

wait
"""
    init_path = staging_dir / "init"
    init_path.write_text(init_script_content, encoding="utf-8")
    init_path.chmod(0o755)

    # Set permissions across staging tree
    for root, dirs, files in os.walk(staging_dir):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for f in files:
            p = Path(root) / f
            if not p.is_symlink():
                cur = p.stat().st_mode
                if cur & stat.S_IXUSR:
                    p.chmod(0o755)
                else:
                    p.chmod(0o644)

    # The generic tree pass above intentionally makes OS content readable, but
    # session-owned content must never inherit that policy.  Boot provisioning
    # assigns these paths to the session UID; staging keeps them root-owned
    # until then, yet already private so an interrupted first boot cannot
    # expose profile or app-container contents.
    private_session_directories = [
        staging_dir / "Users" / "Rei",
        staging_dir / "Users" / "Rei" / "Applications",
        staging_dir / "Users" / "Rei" / "Desktop",
        staging_dir / "Users" / "Rei" / "Documents",
        staging_dir / "Users" / "Rei" / "Downloads",
        staging_dir / "Users" / "Rei" / "Library",
        staging_dir / "Users" / "Rei" / "Library" / "Containers",
        staging_dir / "Users" / "Rei" / "Library" / "Containers" / "org.lcl.terminal",
        staging_dir / "Users" / "Rei" / "Library" / "Containers" / "org.lcl.terminal" / "Data",
        staging_dir / "Users" / "Rei" / "Library" / "Containers" / "org.lcl.terminal" / "Cache",
        staging_dir / "Users" / "Rei" / "Library" / "Containers" / "org.lcl.terminal" / "Preferences",
        staging_dir / "Users" / "Rei" / "Library" / "Containers" / "org.lcl.terminal" / "Temporary",
        staging_dir / "Users" / "Rei" / "Movies",
        staging_dir / "Users" / "Rei" / "Music",
        staging_dir / "Users" / "Rei" / "Pictures",
    ]
    for directory in private_session_directories:
        if directory.is_symlink() or not directory.is_dir():
            raise RuntimeError(f"Canonical private session path is unsafe: {directory}")
        directory.chmod(0o700)
    # This is deliberately not session-owned storage.  sandboxd needs a
    # root-controlled parent in which it can create app-UID-owned containers;
    # the session user gets traversal only for its trusted Terminal container.
    (staging_dir / "Users" / "Rei" / "Library" / "Containers").chmod(0o711)
    security_state = staging_dir / "var" / "lib" / "lcl-security"
    if security_state.is_symlink() or not security_state.is_dir():
        raise RuntimeError(f"Canonical security state path is unsafe: {security_state}")
    security_state.chmod(0o700)
    for private_file in (staging_dir / "Users" / "Rei" / ".bashrc",
                         staging_dir / "Users" / "Rei" / ".profile"):
        if private_file.is_symlink() or not private_file.is_file():
            raise RuntimeError(f"Canonical private session file is unsafe: {private_file}")
        private_file.chmod(0o600)

    return sha_map


def _is_visible_manifest_text(value: object, maximum_length: int) -> bool:
    return (
        isinstance(value, str)
        and 0 < len(value) <= maximum_length
        and "\x00" not in value
        and all(ord(character) >= 0x20 and ord(character) != 0x7F for character in value)
    )


def _is_valid_dotted_identifier(value: object, maximum_length: int = 128) -> bool:
    if not _is_visible_manifest_text(value, maximum_length):
        return False

    assert isinstance(value, str)
    labels = value.split(".")
    if len(labels) < 2:
        return False
    for label in labels:
        if not label or not ("a" <= label[0] <= "z") or label.endswith("-"):
            return False
        if any(not ("a" <= character <= "z" or "0" <= character <= "9" or character == "-")
               for character in label):
            return False
    return True


def _is_safe_bundle_relative_path(value: object, required_first_component: str) -> bool:
    if not _is_visible_manifest_text(value, 512):
        return False

    assert isinstance(value, str)
    if "\\" in value or "//" in value:
        return False
    path = Path(value)
    return (
        not path.is_absolute()
        and bool(path.parts)
        and path.parts[0] == required_first_component
        and all(component not in ("", ".", "..") for component in path.parts)
    )


def _resolve_bundle_file(bundle_root: Path, relative_path: str) -> Path | None:
    try:
        candidate = bundle_root
        for component in Path(relative_path).parts:
            candidate /= component
            if candidate.is_symlink():
                return None
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(bundle_root)
    except (OSError, RuntimeError, ValueError):
        return None
    if not resolved.is_file() or resolved.stat().st_nlink != 1:
        return None
    return resolved


def _manifest_object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    manifest: dict[str, object] = {}
    for key, value in pairs:
        if key in manifest:
            raise ValueError(f"duplicate key '{key}'")
        manifest[key] = value
    return manifest


def validate_app_bundles(staging_dir: Path) -> None:
    """Strictly validates all .app bundles in the canonical application scopes."""
    apps_dirs = [
        staging_dir / "System" / "Applications",
        staging_dir / "Applications",
        staging_dir / "Users" / "Rei" / "Applications",
    ]

    validated_count = 0
    seen_app_ids: dict[str, str] = {}

    for parent in apps_dirs:
        if not parent.is_dir():
            continue
        for app_dir in parent.iterdir():
            if not app_dir.is_dir() or not app_dir.name.endswith(".app"):
                continue
            if app_dir.is_symlink():
                raise RuntimeError(f"Bundle {app_dir.name} must not be a symlink")

            try:
                bundle_root = app_dir.resolve(strict=True)
            except OSError as ex:
                raise RuntimeError(f"Could not resolve bundle {app_dir.name}: {ex}")

            manifest_file = app_dir / "Manifest.json"
            if manifest_file.is_symlink() or not manifest_file.is_file():
                raise RuntimeError(f"Bundle {app_dir.name} in {parent.name} is missing Manifest.json")

            resources_dir = app_dir / "Resources"
            if resources_dir.is_symlink() or not resources_dir.is_dir():
                raise RuntimeError(f"Bundle {app_dir.name} is missing Resources directory")
            try:
                resources_root = resources_dir.resolve(strict=True)
                resources_root.relative_to(bundle_root)
            except (OSError, RuntimeError, ValueError):
                raise RuntimeError(f"Bundle {app_dir.name} Resources directory escapes its bundle")

            executables_dir = app_dir / "Executables"
            if executables_dir.is_symlink() or not executables_dir.is_dir():
                raise RuntimeError(f"Bundle {app_dir.name} is missing Executables directory")
            try:
                executables_root = executables_dir.resolve(strict=True)
                executables_root.relative_to(bundle_root)
            except (OSError, RuntimeError, ValueError):
                raise RuntimeError(f"Bundle {app_dir.name} Executables directory escapes its bundle")

            try:
                manifest = json.loads(
                    manifest_file.read_text(encoding="utf-8"),
                    object_pairs_hook=_manifest_object_without_duplicate_keys,
                )
            except Exception as ex:
                raise RuntimeError(f"Invalid JSON in {manifest_file}: {ex}")
            if not isinstance(manifest, dict):
                raise RuntimeError(f"Manifest for {app_dir.name} must be a JSON object")

            app_id = manifest.get("id")
            if not _is_valid_dotted_identifier(app_id):
                raise RuntimeError(f"Bundle {app_dir.name} has an invalid 'id' field in Manifest.json")

            assert isinstance(app_id, str)

            if app_id in seen_app_ids:
                log(f"  ⚠ Duplicate app ID '{app_id}' found in {parent.name}; keeping higher-priority entry from {seen_app_ids[app_id]}")
            else:
                seen_app_ids[app_id] = parent.name

            app_name = manifest.get("name")
            if not _is_visible_manifest_text(app_name, 256):
                raise RuntimeError(f"Bundle {app_dir.name} has an invalid 'name' field in Manifest.json")

            icon_rel = manifest.get("icon")
            if not _is_safe_bundle_relative_path(icon_rel, "Resources"):
                raise RuntimeError(
                    f"Bundle {app_dir.name} must declare an icon under Resources/"
                )
            assert isinstance(icon_rel, str)
            icon_path = _resolve_bundle_file(bundle_root, icon_rel)
            if icon_path is None or not icon_path.is_relative_to(resources_root):
                raise RuntimeError(f"Bundle {app_dir.name} icon not found inside Resources/")

            exec_ref = manifest.get("executable")
            if not _is_safe_bundle_relative_path(exec_ref, "Executables"):
                raise RuntimeError(
                    f"Bundle {app_dir.name} must declare an executable under Executables/"
                )
            assert isinstance(exec_ref, str)
            target_exec = _resolve_bundle_file(bundle_root, exec_ref)
            if target_exec is None or not target_exec.is_relative_to(executables_root):
                raise RuntimeError(f"Bundle {app_dir.name} executable not found inside Executables/")

            version = manifest.get("version")
            if version is not None and not _is_visible_manifest_text(version, 128):
                raise RuntimeError(f"Bundle {app_dir.name} has an invalid optional 'version' field")

            runtime = manifest.get("runtime")
            if runtime is not None and not _is_visible_manifest_text(runtime, 128):
                raise RuntimeError(f"Bundle {app_dir.name} has an invalid optional 'runtime' field")
            bundle_type = manifest.get("type", "gui")
            if bundle_type not in ("gui", "cli"):
                raise RuntimeError(f"Bundle {app_dir.name} has an invalid 'type' field")

            requested_permissions = manifest.get("requestedPermissions", [])
            if not isinstance(requested_permissions, list) or any(
                not _is_valid_dotted_identifier(permission) for permission in requested_permissions
            ):
                raise RuntimeError(f"Bundle {app_dir.name} has invalid requestedPermissions")
            if len(requested_permissions) != len(set(requested_permissions)):
                raise RuntimeError(f"Bundle {app_dir.name} requests a permission more than once")

            if runtime == "org.lcl.javascript" or exec_ref.endswith(".js"):
                interp = staging_dir / "System" / "Core" / "lcl-js"
                if not interp.is_file():
                    interp = staging_dir / "usr" / "bin" / "lcl-js"
                if not interp.is_file():
                    raise RuntimeError(f"Bundle {app_dir.name} requires JavaScript runtime, but interpreter not found.")
                if not os.access(str(interp), os.X_OK):
                    raise RuntimeError(f"JavaScript runtime interpreter {interp} is not executable.")
            else:
                if not os.access(str(target_exec), os.X_OK):
                    raise RuntimeError(f"Bundle {app_dir.name} executable {target_exec} is not executable (0755).")

            log(f"  ✓ Validated bundle {app_dir.name} in {parent.name} (id={app_id}, exec={exec_ref})")
            validated_count += 1

    if validated_count == 0:
        raise RuntimeError("No app bundles were found or validated in the staging tree.")


def verify_rootfs_image(ext4_path: Path, arch: str = "x86_64") -> None:
    """Verifies that key canonical userspace files exist inside the generated ext4 image."""
    norm_arch = normalize_arch(arch)
    meta = ARCH_META[norm_arch]
    loader_name = meta["loader"]

    log(f"Verifying ext4 filesystem contents of {ext4_path.name} ({norm_arch})...")
    required_files = [
        "/System/Core/lcl-core",
        "/System/Core/lcl-desktop-shell",
        "/System/Core/lcl-mobile-shell",
        "/System/Core/lcl-shell-launcher",
        "/System/Core/lcl-sessiond",
        "/System/Core/lcl-sandboxd",
        "/System/Core/lcl-securityd",
        "/System/Core/lcl-sandbox-probe",
        "/System/Core/lcl-open",
        "/System/Core/lcl-js",
        "/System/Tools/bash",
        "/System/Tools/id",
        f"/System/Library/Libraries/{loader_name}",
        "/System/Applications/Terminal.app/Manifest.json",
        "/System/Applications/Terminal.app/Executables/Terminal",
        "/System/Applications/Terminal.app/Resources/Icon.png",
        "/System/Applications/Sandbox Probe.app/Manifest.json",
        "/System/Applications/Sandbox Probe.app/Executables/SandboxProbe",
        "/System/Applications/Sandbox Probe.app/Resources/Icon.png",
        "/System/Library/Fonts/inter",
        "/System/Library/Gestalt/default.json",
        "/System/Library/Wallpapers/wallpaper.jpg",
        "/System/Library/EGL/glvnd/egl_vendor.d/50_mesa.json",
        "/System/Library/Input/libinput",
        "/Users/Rei/.bashrc",
        "/Users/Rei/Library/Containers/org.lcl.terminal/Data",
        "/usr/share/terminfo/x/xterm-256color",
        "/etc/profile",
        "/init",
    ]

    for req in required_files:
        # debugfs tokenizes its -R command independently of subprocess.  App
        # bundle names may contain spaces (for example, Sandbox Probe.app), so
        # quote the filesystem path for debugfs rather than letting it split
        # the path into multiple command arguments.
        debugfs_path = req.replace("\\", "\\\\").replace('"', '\\"')
        cmd = ["debugfs", "-R", f'stat "{debugfs_path}"', str(ext4_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if "Inode:" not in res.stdout or res.returncode != 0:
            raise AssertionError(f"Required canonical file '{req}' missing from rootfs image {ext4_path}")

    # Do not let the generic tree-wide 0755 normalization regress the private
    # session and app-container boundary.  These files start root-owned in the
    # image and are assigned to the session/app identity by the privileged
    # provisioner at boot, but their mode must already be private in the image.
    private_modes = {
        "/Users/Rei": 0o700,
        "/Users/Rei/.bashrc": 0o600,
        "/Users/Rei/.profile": 0o600,
        "/Users/Rei/Library/Containers": 0o711,
        "/var/lib/lcl-security": 0o700,
        "/Users/Rei/Library/Containers/org.lcl.terminal": 0o700,
        "/Users/Rei/Library/Containers/org.lcl.terminal/Data": 0o700,
        "/Users/Rei/Library/Containers/org.lcl.terminal/Cache": 0o700,
        "/Users/Rei/Library/Containers/org.lcl.terminal/Preferences": 0o700,
        "/Users/Rei/Library/Containers/org.lcl.terminal/Temporary": 0o700,
    }
    for path, expected_mode in private_modes.items():
        stat_result = subprocess.run(
            ["debugfs", "-R", f'stat "{path}"', str(ext4_path)],
            capture_output=True,
            text=True,
            check=True,
        )
        mode_line = next((line for line in stat_result.stdout.splitlines()
                          if "Mode:" in line), "")
        mode_text = mode_line.partition("Mode:")[2].lstrip()
        if not mode_text:
            raise AssertionError(f"Could not read mode for private rootfs path '{path}'")
        try:
            actual_mode = int(mode_text.split()[0], 8) & 0o777
        except ValueError as error:
            raise AssertionError(f"Could not parse mode for private rootfs path '{path}'") from error
        if actual_mode != expected_mode:
            raise AssertionError(
                f"Private rootfs path '{path}' has mode {actual_mode:04o}; expected {expected_mode:04o}"
            )

    # Check Terminal.app Manifest content
    cat_cmd = ["debugfs", "-R", "cat /System/Applications/Terminal.app/Manifest.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    manifest_json = json.loads(cat_res.stdout)
    if manifest_json.get("executable") != "Executables/Terminal":
        raise AssertionError(f"Terminal.app Manifest in rootfs has invalid executable: {manifest_json.get('executable')}")

    gestalt_cat = subprocess.run(
        ["debugfs", "-R", "cat /System/Library/Gestalt/default.json", str(ext4_path)],
        capture_output=True, text=True, check=True,
    )
    gestalt_json = json.loads(gestalt_cat.stdout)
    if gestalt_json.get("version") != 1 or not isinstance(gestalt_json.get("display"), dict):
        raise AssertionError("Default Gestalt in rootfs has an invalid schema")

    # Check /init shebang
    init_cat = subprocess.run(["debugfs", "-R", "cat /init", str(ext4_path)], capture_output=True, text=True, check=True)
    first_line = init_cat.stdout.splitlines()[0] if init_cat.stdout.splitlines() else ""
    if not first_line.startswith("#!/System/Tools/sh"):
        raise AssertionError(f"/init shebang is not canonical #!/System/Tools/sh: {first_line}")

    log(f"✓ All required canonical userspace files, {loader_name}, manifests, and /init entrypoint verified successfully.")


def ensure_binfmt(arch: str) -> None:
    """Ensures QEMU binfmt handlers are registered on Linux for cross-arch Docker execution."""
    if platform.system().lower() != "linux":
        return
    norm_arch = normalize_arch(arch)
    host_arch = normalize_arch(None)
    if host_arch == norm_arch:
        return

    handler_name = "qemu-aarch64" if norm_arch == "aarch64" else "qemu-x86_64"
    handler_path = Path("/proc/sys/fs/binfmt_misc") / handler_name
    if handler_path.exists():
        return

    log(f"Multi-arch binfmt handler for {norm_arch} ({handler_name}) is missing. Registering via tonistiigi/binfmt...")
    try:
        subprocess.run(
            ["docker", "run", "--privileged", "--rm", "tonistiigi/binfmt", "--install", "all"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        log("✓ Registered binfmt handlers successfully.")
    except Exception as ex:
        err(f"Warning: Failed to auto-register binfmt handlers: {ex}")
        err("If cross-platform Docker execution fails with 'exec format error', run:")
        err("  docker run --privileged --rm tonistiigi/binfmt --install all")


def run_inside_docker(
    arch: str = "aarch64",
    image_size_mb: int = 1024,
    force: bool = False,
) -> tuple[Path, Path, dict[str, str]]:
    """Dispatches rootfs generation inside multi-arch Docker container."""
    norm_arch = normalize_arch(arch)
    plat = ARCH_META[norm_arch]["docker_platform"]
    image_tag = f"lcl-os-qemu-builder:{norm_arch}"

    ensure_binfmt(norm_arch)

    log(f"Executing rootfs build inside Docker ({norm_arch}, {plat})...")

    image_fingerprint_path = docker_image_fingerprint_path(norm_arch)
    dockerfile_fingerprint = get_sha256(DOCKERFILE)
    inspect = subprocess.run(
        ["docker", "image", "inspect", image_tag],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    needs_image_build = inspect.returncode != 0
    if not needs_image_build:
        try:
            needs_image_build = (
                not image_fingerprint_path.is_file() or
                image_fingerprint_path.read_text(encoding="utf-8").strip() != dockerfile_fingerprint
            )
        except OSError:
            needs_image_build = True

    if needs_image_build:
        log(f"Building Docker image {image_tag} ({plat}); Dockerfile changed or image is missing...")
        subprocess.run([
            "docker", "build", "--platform", plat, "-t", image_tag, "-f", str(DOCKERFILE), str(SCRIPT_DIR)
        ], check=True)
        try:
            image_fingerprint_path.parent.mkdir(parents=True, exist_ok=True)
            temporary_fingerprint = image_fingerprint_path.with_suffix(".stamp.tmp")
            temporary_fingerprint.write_text(dockerfile_fingerprint + "\n", encoding="utf-8")
            temporary_fingerprint.replace(image_fingerprint_path)
        except OSError as ex:
            log(f"[WARN] Could not persist Dockerfile fingerprint: {ex}")

    image_id = subprocess.check_output(
        ["docker", "image", "inspect", "--format", "{{.Id}}", image_tag],
        text=True,
    ).strip()
    env_args: list[str] = ["-e", f"LCL_QEMU_BUILDER_IMAGE_ID={image_id}"]
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        env_args.extend(["-e", f"HOST_UID={os.getuid()}", "-e", f"HOST_GID={os.getgid()}"])

    cmd = [
        "docker", "run", "--rm",
        "--platform", plat,
        *env_args,
        "-v", f"{PROJECT_ROOT}:/src",
        "-w", "/src",
        image_tag,
        "python3", "tooling/build/build_rootfs.py",
        "--arch", norm_arch,
        "--size", str(image_size_mb),
        "--inside-docker",
    ]
    if force:
        cmd.append("--force")
    subprocess.run(cmd, check=True)

    staging_dir = ROOTFS_BASE_DIR / norm_arch
    out_ext4 = ROOTFS_BASE_DIR / f"lcl-rootfs-{norm_arch}.ext4"
    return staging_dir, out_ext4, {}


def fix_permissions() -> None:
    """Fix ownership and permissions on out/ after a Docker build."""
    if not BUILD_DIR.exists():
        return
    uid = os.environ.get("HOST_UID")
    gid = os.environ.get("HOST_GID")
    if uid and gid:
        try:
            subprocess.run(["chown", "-R", f"{uid}:{gid}", str(BUILD_DIR)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass


def build_rootfs_ext4(
    arch: str = "x86_64",
    image_size_mb: int = 1024,
    inside_docker: bool = False,
    force: bool = False,
) -> tuple[Path, Path, dict[str, str]]:
    """Builds the canonical ext4 rootfs image from the staging tree."""
    norm_arch = normalize_arch(arch)
    # Canonical userspace is always built in the pinned Docker environment.
    # Besides keeping toolchains reproducible, this prevents a CMake cache
    # configured at /src/out inside Docker from being reopened through the
    # host checkout's absolute path.
    if not inside_docker:
        return run_inside_docker(norm_arch, image_size_mb, force=force)

    ROOTFS_BASE_DIR.mkdir(parents=True, exist_ok=True)
    staging_dir = ROOTFS_BASE_DIR / norm_arch
    out_ext4 = ROOTFS_BASE_DIR / f"lcl-rootfs-{norm_arch}.ext4"
    target_build_dir = canonical_build_dir(norm_arch)
    if force and target_build_dir.exists():
        log(f"Removing canonical QEMU build tree for forced rebuild: {target_build_dir}")
        shutil.rmtree(target_build_dir)
    binaries = ensure_binaries(norm_arch)
    fingerprint = rootfs_input_fingerprint(norm_arch, binaries)
    fingerprint_path = rootfs_fingerprint_path(norm_arch)
    cache_allowed = inside_docker and bool(os.environ.get("LCL_QEMU_BUILDER_IMAGE_ID"))

    if cache_allowed and not force and out_ext4.is_file() and fingerprint_path.is_file():
        if fingerprint_path.read_text(encoding="utf-8").strip() == fingerprint:
            log(f"Reusing current {out_ext4.name}; packaged inputs are unchanged.")
            return staging_dir, out_ext4, {}

    sha_map = stage_canonical_rootfs(staging_dir, norm_arch, binaries=binaries)

    log(f"Creating ext4 rootfs image at {out_ext4} ({image_size_mb} MB)...")
    if out_ext4.exists():
        out_ext4.unlink()

    # Create sparse/empty file
    subprocess.run(["truncate", "-s", f"{image_size_mb}M", str(out_ext4)], check=True)

    # Format and populate directory contents via mkfs.ext4 -d
    mkfs_cmd = [
        "mkfs.ext4",
        "-F",
        "-L", "lcl-rootfs",
        "-d", str(staging_dir),
        str(out_ext4),
    ]
    res = subprocess.run(mkfs_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        err(f"mkfs.ext4 failed:\n{res.stderr}")
        raise RuntimeError("Failed to build ext4 rootfs image")

    img_size = out_ext4.stat().st_size
    log(f"✓ Built {out_ext4.name} ({img_size} bytes / {img_size / (1024*1024):.1f} MB)")

    # Verify content inside ext4 image using debugfs
    verify_rootfs_image(out_ext4, norm_arch)

    if cache_allowed:
        fingerprint_path.parent.mkdir(parents=True, exist_ok=True)
        temporary_fingerprint = fingerprint_path.with_suffix(".sha256.tmp")
        temporary_fingerprint.write_text(fingerprint + "\n", encoding="utf-8")
        temporary_fingerprint.replace(fingerprint_path)

    if inside_docker:
        fix_permissions()

    return staging_dir, out_ext4, sha_map


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux - Canonical RootFS Builder")
    parser.add_argument("--arch", default="x86_64", help="Target architecture (x86_64 or aarch64)")
    parser.add_argument("--size", type=int, default=1024, help="Filesystem size in MB (default: 1024)")
    parser.add_argument("--inside-docker", action="store_true", help="Internal flag when running inside container")
    parser.add_argument("--force", action="store_true", help="Ignore cached input fingerprints and rebuild the rootfs")
    args = parser.parse_args()

    norm_arch = normalize_arch(args.arch)
    staging_dir, out_ext4, sha_map = build_rootfs_ext4(
        arch=norm_arch,
        image_size_mb=args.size,
        inside_docker=args.inside_docker,
        force=args.force,
    )
    print("\n--- Summary ---")
    print(f"Target architecture: {norm_arch}")
    print(f"Staging tree: {staging_dir}")
    print(f"RootFS image: {out_ext4} ({out_ext4.stat().st_size} bytes)")
    for k, v in sha_map.items():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
