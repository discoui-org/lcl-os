#!/usr/bin/env python3
"""
LCL Core Linux - Canonical RootFS Artifact Builder (Stage 4C.2d)
Canonical LCL Filesystem & Application ABI v1

Builds a single, deterministic, platform-independent ext4 root filesystem artifact:
    build/rootfs/lcl-rootfs-<arch>.ext4
from the canonical staging tree:
    build/rootfs/<arch>/

Canonical Root Namespace:
    /Applications/              # Machine-wide installed applications
    /Library/                   # Machine-wide mutable resources & config
    /Runtime/                   # Boot & session runtime state (/Runtime/Sessions/Rei, /Runtime/Temporary)
    /System/                    # Immutable OS content (Applications, Core, Tools, Library)
        /System/Applications/   # Built-in .app bundles (Terminal.app, UIDemo.app, UIDemoJS.app)
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
PROJECT_ROOT = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_ROOT / "build"
ROOTFS_BASE_DIR = BUILD_DIR / "rootfs"
DOCKERFILE = SCRIPT_DIR / "Dockerfile.qemu"

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
    return BUILD_DIR / "rootfs-build" / normalize_arch(arch)


def find_built_binary(name: str, arch: str | None = None) -> Path | None:
    roots = [canonical_build_dir(arch)] if arch is not None else []
    roots.append(BUILD_DIR)
    for root in roots:
        for cand in [
            root / name,
            root / "apps" / "ui_demo" / name,
            root / "apps" / name / name,
            root / "src" / "tools" / name,
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
    targets = [
        "lcl-desktop-shell",
        "lcl-mobile-shell",
        "lcl-shell-launcher",
        "lcl-terminal",
        "lcl-sessiond",
        "lcl-open",
        "lcl-core",
        "lcl-js",
        "lcl_ui_demo",
    ]

    # Existence alone is not freshness. Always let CMake's dependency graph
    # update these targets so a protocol/header change cannot be packaged with
    # stale canonical userspace binaries.
    target_build_dir = canonical_build_dir(norm_arch)
    if not (target_build_dir / "CMakeCache.txt").is_file():
        subprocess.run(
            ["cmake", "-B", str(target_build_dir), "-S", str(PROJECT_ROOT)],
            check=True,
        )
    log(f"Updating canonical targets for {norm_arch}...")
    subprocess.run(
        ["cmake", "--build", str(target_build_dir), "--target"] +
        targets + ["-j", str(os.cpu_count() or 4)],
        check=True,
    )

    binaries: dict[str, Path] = {}
    for name in targets:
        p = find_built_binary(name, norm_arch)
        if not p or not p.is_file():
            raise RuntimeError(f"Required canonical binary '{name}' was not found or is not {norm_arch} after build.")
        binaries[name] = p

    return binaries


def stage_canonical_rootfs(staging_dir: Path, arch: str = "x86_64") -> dict[str, str]:
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
        "Users/Rei/Library/Containers/org.lcl.uidemo/Data",
        "Users/Rei/Library/Containers/org.lcl.uidemo/Cache",
        "Users/Rei/Library/Containers/org.lcl.uidemo/Preferences",
        "Users/Rei/Library/Containers/org.lcl.uidemo/Temporary",
        "Users/Rei/Library/Containers/org.lcl.uidemo-js/Data",
        "Users/Rei/Library/Containers/org.lcl.uidemo-js/Cache",
        "Users/Rei/Library/Containers/org.lcl.uidemo-js/Preferences",
        "Users/Rei/Library/Containers/org.lcl.uidemo-js/Temporary",
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
    binaries = ensure_binaries(norm_arch)
    sha_map: dict[str, str] = {}
    core_daemons = {
        "lcl-desktop-shell": dest_system_core / "lcl-desktop-shell",
        "lcl-mobile-shell": dest_system_core / "lcl-mobile-shell",
        "lcl-shell-launcher": dest_system_core / "lcl-shell-launcher",
        "lcl-sessiond": dest_system_core / "lcl-sessiond",
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
        "mount", "mkdir", "sleep", "ls", "cat", "uname", "grep",
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
    for wp_name in ["wallpaper.jpg", "wallpaper.png"]:
        wp_src = PROJECT_ROOT / wp_name
        if wp_src.is_file():
            shutil.copy2(wp_src, dest_system_wallpapers / wp_name)

    gestalt_src = PROJECT_ROOT / "config" / "gestalt" / "default.json"
    if not gestalt_src.is_file():
        raise RuntimeError(f"Missing default Gestalt: {gestalt_src}")
    shutil.copy2(gestalt_src, dest_system_gestalt / "default.json")

    # 11. Built-in App Bundles ABI v1 (/System/Applications/)
    # (A) Terminal.app
    term_dst = dest_system_apps / "Terminal.app"
    term_dst.mkdir(parents=True, exist_ok=True)
    (term_dst / "Executables").mkdir(parents=True, exist_ok=True)
    (term_dst / "Resources").mkdir(parents=True, exist_ok=True)

    term_manifest = PROJECT_ROOT / "src" / "apps" / "terminal" / "Manifest.json"
    term_icon = PROJECT_ROOT / "src" / "apps" / "terminal" / "Resources" / "Icon.png"
    if not term_manifest.is_file() or not term_icon.is_file():
        raise RuntimeError(f"Missing Terminal.app source files in {PROJECT_ROOT / 'src' / 'apps' / 'terminal'}")
    shutil.copy2(term_manifest, term_dst / "Manifest.json")
    shutil.copy2(term_icon, term_dst / "Resources" / "Icon.png")

    term_bin = binaries["lcl-terminal"]
    shutil.copy2(term_bin, term_dst / "Executables" / "Terminal")
    (term_dst / "Executables" / "Terminal").chmod(0o755)
    sha_map["Terminal.app"] = get_sha256(term_dst / "Executables" / "Terminal")
    copy_ldd_deps(term_dst / "Executables" / "Terminal", dest_system_lib)

    # (B) UIDemo.app
    uidemo_dst = dest_system_apps / "UIDemo.app"
    uidemo_dst.mkdir(parents=True, exist_ok=True)
    (uidemo_dst / "Executables").mkdir(parents=True, exist_ok=True)
    (uidemo_dst / "Resources").mkdir(parents=True, exist_ok=True)

    uidemo_manifest = PROJECT_ROOT / "apps" / "ui_demo" / "Manifest.json"
    uidemo_icon = PROJECT_ROOT / "apps" / "ui_demo" / "Resources" / "Icon.png"
    if not uidemo_manifest.is_file() or not uidemo_icon.is_file():
        raise RuntimeError(f"Missing UIDemo.app source files in {PROJECT_ROOT / 'apps' / 'ui_demo'}")
    shutil.copy2(uidemo_manifest, uidemo_dst / "Manifest.json")
    shutil.copy2(uidemo_icon, uidemo_dst / "Resources" / "Icon.png")

    uidemo_bin = binaries["lcl_ui_demo"]
    shutil.copy2(uidemo_bin, uidemo_dst / "Executables" / "UIDemo")
    (uidemo_dst / "Executables" / "UIDemo").chmod(0o755)
    sha_map["UIDemo.app"] = get_sha256(uidemo_dst / "Executables" / "UIDemo")
    copy_ldd_deps(uidemo_dst / "Executables" / "UIDemo", dest_system_lib)

    # (C) UIDemoJS.app
    uidemojs_dst = dest_system_apps / "UIDemoJS.app"
    uidemojs_dst.mkdir(parents=True, exist_ok=True)
    (uidemojs_dst / "Executables").mkdir(parents=True, exist_ok=True)
    (uidemojs_dst / "Resources").mkdir(parents=True, exist_ok=True)

    uidemojs_manifest = PROJECT_ROOT / "apps" / "ui_demo_js" / "Manifest.json"
    uidemojs_icon = PROJECT_ROOT / "apps" / "ui_demo_js" / "Resources" / "Icon.png"
    uidemojs_main = PROJECT_ROOT / "apps" / "ui_demo_js" / "main.js"
    if not uidemojs_manifest.is_file() or not uidemojs_icon.is_file() or not uidemojs_main.is_file():
        raise RuntimeError(f"Missing UIDemoJS.app source files in {PROJECT_ROOT / 'apps' / 'ui_demo_js'}")
    shutil.copy2(uidemojs_manifest, uidemojs_dst / "Manifest.json")
    shutil.copy2(uidemojs_icon, uidemojs_dst / "Resources" / "Icon.png")
    shutil.copy2(uidemojs_main, uidemojs_dst / "Executables" / "Main.js")
    (uidemojs_dst / "Executables" / "Main.js").chmod(0o755)
    sha_map["UIDemoJS.app"] = get_sha256(uidemojs_dst / "Executables" / "Main.js")

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

# Start Compositor Display Server
/System/Core/lcl-core 2>&1 | tee /var/log/lcl_compositor.log &
sleep 0.2

# Start LCL Session Daemon (sole application launch authority)
if [ -x /System/Core/lcl-sessiond ]; then
    echo "[init] Starting lcl-sessiond..."
    /System/Core/lcl-sessiond 2>&1 | tee /var/log/lcl_sessiond.log &
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

    return sha_map


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

            manifest_file = app_dir / "Manifest.json"
            if not manifest_file.is_file():
                manifest_file = app_dir / "metadata.json"
                if not manifest_file.is_file():
                    raise RuntimeError(f"Bundle {app_dir.name} in {parent.name} is missing Manifest.json")

            try:
                manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
            except Exception as ex:
                raise RuntimeError(f"Invalid JSON in {manifest_file}: {ex}")

            app_id = manifest.get("id") or manifest.get("appId")
            if not app_id:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'id' field in Manifest.json")

            if app_id in seen_app_ids:
                log(f"  ⚠ Duplicate app ID '{app_id}' found in {parent.name}; keeping higher-priority entry from {seen_app_ids[app_id]}")
            else:
                seen_app_ids[app_id] = parent.name

            app_name = manifest.get("name")
            if not app_name:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'name' field in Manifest.json")

            icon_rel = manifest.get("icon")
            if icon_rel and not (app_dir / icon_rel).is_file():
                raise RuntimeError(f"Bundle {app_dir.name} icon not found: {app_dir / icon_rel}")

            exec_ref = manifest.get("executable") or manifest.get("exec")
            if not exec_ref:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'executable' field in Manifest.json")

            if exec_ref.startswith("/"):
                target_exec = staging_dir / exec_ref.lstrip("/")
            else:
                target_exec = app_dir / exec_ref

            if not target_exec.is_file():
                raise RuntimeError(f"Bundle {app_dir.name} resolved executable not found: {target_exec}")

            runtime = manifest.get("runtime")
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
        "/System/Core/lcl-open",
        "/System/Core/lcl-js",
        "/System/Tools/bash",
        f"/System/Library/Libraries/{loader_name}",
        "/System/Applications/Terminal.app/Manifest.json",
        "/System/Applications/Terminal.app/Executables/Terminal",
        "/System/Applications/Terminal.app/Resources/Icon.png",
        "/System/Applications/UIDemo.app/Manifest.json",
        "/System/Applications/UIDemo.app/Executables/UIDemo",
        "/System/Applications/UIDemoJS.app/Manifest.json",
        "/System/Applications/UIDemoJS.app/Executables/Main.js",
        "/System/Library/Fonts/inter",
        "/System/Library/Gestalt/default.json",
        "/System/Library/Wallpapers/wallpaper.jpg",
        "/System/Library/EGL/glvnd/egl_vendor.d/50_mesa.json",
        "/System/Library/Input/libinput",
        "/Users/Rei/.bashrc",
        "/Users/Rei/Library/Containers/org.lcl.terminal/Data",
        "/etc/profile",
        "/init",
    ]

    for req in required_files:
        cmd = ["debugfs", "-R", f"stat {req}", str(ext4_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if "Inode:" not in res.stdout or res.returncode != 0:
            raise AssertionError(f"Required canonical file '{req}' missing from rootfs image {ext4_path}")

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

    # Check UIDemo.app Manifest content
    cat_cmd = ["debugfs", "-R", "cat /System/Applications/UIDemo.app/Manifest.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    manifest_json = json.loads(cat_res.stdout)
    if manifest_json.get("executable") != "Executables/UIDemo":
        raise AssertionError(f"UIDemo.app Manifest in rootfs has invalid executable: {manifest_json.get('executable')}")

    # Check UIDemoJS.app Manifest content
    cat_cmd = ["debugfs", "-R", "cat /System/Applications/UIDemoJS.app/Manifest.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    manifest_json = json.loads(cat_res.stdout)
    if manifest_json.get("executable") != "Executables/Main.js" or manifest_json.get("runtime") != "org.lcl.javascript":
        raise AssertionError(f"UIDemoJS.app Manifest in rootfs has invalid metadata: {manifest_json}")

    # Check /init shebang
    init_cat = subprocess.run(["debugfs", "-R", "cat /init", str(ext4_path)], capture_output=True, text=True, check=True)
    first_line = init_cat.stdout.splitlines()[0] if init_cat.stdout.splitlines() else ""
    if not first_line.startswith("#!/System/Tools/sh"):
        raise AssertionError(f"/init shebang is not canonical #!/System/Tools/sh: {first_line}")

    log(f"✓ All required canonical userspace files, {loader_name}, manifests, and /init entrypoint verified successfully.")


def run_inside_docker(arch: str = "aarch64", image_size_mb: int = 1024) -> tuple[Path, Path, dict[str, str]]:
    """Dispatches rootfs generation inside multi-arch Docker container."""
    norm_arch = normalize_arch(arch)
    plat = ARCH_META[norm_arch]["docker_platform"]
    image_tag = f"lcl-os-qemu-builder:{norm_arch}"

    log(f"Executing rootfs build inside Docker ({norm_arch}, {plat})...")

    # Build image if missing
    inspect = subprocess.run(["docker", "image", "inspect", image_tag], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if inspect.returncode != 0:
        log(f"Building Docker image {image_tag} ({plat})...")
        subprocess.run([
            "docker", "build", "--platform", plat, "-t", image_tag, "-f", str(DOCKERFILE), str(SCRIPT_DIR)
        ], check=True)

    env_args: list[str] = []
    if hasattr(os, "getuid") and hasattr(os, "getgid"):
        env_args = ["-e", f"HOST_UID={os.getuid()}", "-e", f"HOST_GID={os.getgid()}"]

    cmd = [
        "docker", "run", "--rm",
        "--platform", plat,
        *env_args,
        "-v", f"{PROJECT_ROOT}:/src",
        "-w", "/src",
        image_tag,
        "python3", "scripts/build_rootfs.py",
        "--arch", norm_arch,
        "--size", str(image_size_mb),
        "--inside-docker",
    ]
    subprocess.run(cmd, check=True)

    staging_dir = ROOTFS_BASE_DIR / norm_arch
    out_ext4 = ROOTFS_BASE_DIR / f"lcl-rootfs-{norm_arch}.ext4"
    return staging_dir, out_ext4, {}


def fix_permissions() -> None:
    """Fix ownership and permissions on build/ inside Docker."""
    if not BUILD_DIR.exists():
        return
    uid = os.environ.get("HOST_UID")
    gid = os.environ.get("HOST_GID")
    if uid and gid:
        try:
            subprocess.run(["chown", "-R", f"{uid}:{gid}", str(BUILD_DIR)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass


def build_rootfs_ext4(arch: str = "x86_64", image_size_mb: int = 1024, inside_docker: bool = False) -> tuple[Path, Path, dict[str, str]]:
    """Builds the canonical ext4 rootfs image from the staging tree."""
    norm_arch = normalize_arch(arch)
    host_arch = normalize_arch(platform.machine())

    # If cross-building on host machine (e.g. host is x86_64, target is aarch64) and not already inside docker, delegate to Docker
    if not inside_docker and host_arch != norm_arch:
        return run_inside_docker(norm_arch, image_size_mb)

    ROOTFS_BASE_DIR.mkdir(parents=True, exist_ok=True)
    staging_dir = ROOTFS_BASE_DIR / norm_arch
    out_ext4 = ROOTFS_BASE_DIR / f"lcl-rootfs-{norm_arch}.ext4"

    sha_map = stage_canonical_rootfs(staging_dir, norm_arch)

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

    if inside_docker:
        fix_permissions()

    return staging_dir, out_ext4, sha_map


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux - Canonical RootFS Builder")
    parser.add_argument("--arch", default="x86_64", help="Target architecture (x86_64 or aarch64)")
    parser.add_argument("--size", type=int, default=1024, help="Filesystem size in MB (default: 1024)")
    parser.add_argument("--inside-docker", action="store_true", help="Internal flag when running inside container")
    args = parser.parse_args()

    norm_arch = normalize_arch(args.arch)
    staging_dir, out_ext4, sha_map = build_rootfs_ext4(arch=norm_arch, image_size_mb=args.size, inside_docker=args.inside_docker)
    print("\n--- Summary ---")
    print(f"Target architecture: {norm_arch}")
    print(f"Staging tree: {staging_dir}")
    print(f"RootFS image: {out_ext4} ({out_ext4.stat().st_size} bytes)")
    for k, v in sha_map.items():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
