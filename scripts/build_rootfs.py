#!/usr/bin/env python3
"""
LCL Core Linux - Canonical RootFS Artifact Builder (Stage 4C.1)

Builds a single, deterministic, platform-independent ext4 root filesystem artifact:
    build/rootfs/lcl-rootfs-x86_64.ext4
from the canonical staging tree:
    build/rootfs/x86_64/

Contains the standard LCL userspace contract:
    /bin -> usr/bin
    /lib64 -> usr/lib
    /usr/bin/{lcl-core, lcl-sessiond, lcl-desktop-shell, lcl-terminal, lcl-open, bash, ...}
    /usr/lib/{ld-linux-x86-64.so.2, libc.so.6, libreadline.so.8, ...}
    /usr/share/lcl/apps/{Terminal.app, UIDemo.app, UIDemoJS.app}
    /usr/share/fonts/
    /usr/share/wallpapers/
    /etc/profile
    /home/user/{.bashrc, .profile}
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_ROOT / "build"
ROOTFS_BASE_DIR = BUILD_DIR / "rootfs"


def log(msg: str) -> None:
    print(f"[LCL ROOTFS] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL ROOTFS ERROR] {msg}", file=sys.stderr, flush=True)


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


def find_built_binary(name: str) -> Path | None:
    for cand in [
        BUILD_DIR / name,
        BUILD_DIR / "apps" / "ui_demo" / name,
        BUILD_DIR / "apps" / name / name,
        BUILD_DIR / "src" / "tools" / name,
    ]:
        if cand.is_file():
            return cand
    return None


def ensure_binaries(arch: str = "x86_64") -> dict[str, Path]:
    """Ensures that required canonical LCL application and tool binaries exist."""
    targets = [
        "lcl-desktop-shell",
        "lcl-terminal",
        "lcl-sessiond",
        "lcl-open",
        "lcl-core",
        "lcl-js",
        "lcl_ui_demo",
    ]

    missing = [name for name in targets if not find_built_binary(name)]
    if missing:
        log(f"Building missing canonical targets: {missing}...")
        if not (BUILD_DIR / "CMakeCache.txt").is_file():
            subprocess.run(["cmake", "-B", str(BUILD_DIR), "-S", str(PROJECT_ROOT)], check=True)
        subprocess.run(
            ["cmake", "--build", str(BUILD_DIR), "--target"] + missing + ["-j", str(os.cpu_count() or 4)],
            check=True,
        )

    binaries: dict[str, Path] = {}
    for name in targets:
        p = find_built_binary(name)
        if not p or not p.is_file():
            raise RuntimeError(f"Required canonical binary '{name}' was not found after build.")
        binaries[name] = p

    return binaries


def stage_canonical_rootfs(staging_dir: Path, arch: str = "x86_64") -> dict[str, str]:
    """Populates the deterministic canonical userspace filesystem layout."""
    log(f"Staging canonical LCL userspace at {staging_dir} ({arch})...")
    if staging_dir.exists():
        shutil.rmtree(staging_dir)

    # 1. Directory hierarchy
    subdirs = [
        "proc",
        "sys",
        "dev",
        "tmp",
        "etc",
        "var/log",
        "usr/bin",
        "usr/lib",
        "usr/share",
        "usr/share/fonts",
        "usr/share/wallpapers",
        "usr/share/lcl/apps",
        "home/user/Desktop",
        "home/user/Documents",
        "home/user/Downloads",
        "home/user/Applications",
        "run/user/1000",
        "run/user/0",
    ]
    for sub in subdirs:
        (staging_dir / sub).mkdir(parents=True, exist_ok=True)

    # 2. Canonical symlinks at /
    for link, target in [
        ("bin", "usr/bin"),
        ("sbin", "usr/bin"),
        ("lib", "usr/lib"),
        ("lib64", "usr/lib"),
    ]:
        link_path = staging_dir / link
        if not link_path.exists():
            link_path.symlink_to(target)

    dest_bin = staging_dir / "usr" / "bin"
    dest_lib = staging_dir / "usr" / "lib"
    dest_share = staging_dir / "usr" / "share"

    # 3. Dynamic linker
    loader_found = False
    for loader_cand in [
        Path("/lib64/ld-linux-x86-64.so.2"),
        Path("/usr/lib64/ld-linux-x86-64.so.2"),
        Path("/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
        Path("/usr/lib/ld-linux-x86-64.so.2"),
    ]:
        if loader_cand.is_file():
            shutil.copy2(loader_cand.resolve(), dest_lib / "ld-linux-x86-64.so.2")
            loader_found = True
            break
    if not loader_found:
        raise RuntimeError("Host dynamic linker ld-linux-x86-64.so.2 not found.")

    # 4. Canonical LCL binaries
    binaries = ensure_binaries(arch)
    sha_map: dict[str, str] = {}
    for name, bin_path in binaries.items():
        dst = dest_bin / name
        shutil.copy2(bin_path, dst)
        dst.chmod(0o755)
        sha_map[name] = get_sha256(dst)
        copy_ldd_deps(dst, dest_lib)

    # Standard open symlink
    if (dest_bin / "lcl-open").is_file():
        open_sym = dest_bin / "open"
        if open_sym.exists() or open_sym.is_symlink():
            open_sym.unlink()
        open_sym.symlink_to("lcl-open")

    # 5. GNU Bash and essential utilities
    bash_src = find_host_bin("bash") or find_host_bin("sh")
    if not bash_src:
        raise RuntimeError("Host bash binary not found.")
    shutil.copy2(bash_src, dest_bin / "bash")
    (dest_bin / "bash").chmod(0o755)
    sha_map["bash"] = get_sha256(dest_bin / "bash")
    copy_ldd_deps(dest_bin / "bash", dest_lib)

    # Symlink /bin/sh -> bash if not present
    sh_sym = dest_bin / "sh"
    if not sh_sym.exists():
        sh_sym.symlink_to("bash")

    util_names = [
        "mount", "mkdir", "sleep", "ls", "cat", "uname", "grep",
        "printf", "dmesg", "tee", "find", "cp", "mv", "rm", "chmod",
        "chown", "touch", "wc", "head", "tail", "clear"
    ]
    for u in util_names:
        u_src = find_host_bin(u)
        if u_src and u_src.is_file():
            u_dst = dest_bin / u
            shutil.copy2(u_src, u_dst)
            u_dst.chmod(0o755)
            copy_ldd_deps(u_dst, dest_lib)

    # Helper script for clear if missing
    if not (dest_bin / "clear").is_file():
        (dest_bin / "clear").write_text("#!/bin/sh\nprintf \"\\033[2J\\033[H\"\n")
        (dest_bin / "clear").chmod(0o755)

    # 6. Graphics & EGL drivers (Mesa DRI, GBM, GLVND, libinput)
    dri_dirs = [
        Path("/usr/lib/x86_64-linux-gnu/dri"),
        Path("/usr/lib/dri"),
        Path("/usr/lib64/dri"),
    ]
    dri_dst = dest_lib / "dri"
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
                        copy_ldd_deps(dri_item, dest_lib)
            break

    gbm_dirs = [
        Path("/usr/lib/x86_64-linux-gnu/gbm"),
        Path("/usr/lib/gbm"),
        Path("/usr/lib64/gbm"),
    ]
    gbm_dst = dest_lib / "gbm"
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
                        copy_ldd_deps(gbm_item, dest_lib)
            break

    # Copy Mesa backend libraries, libgbm, libEGL, libglapi into dest_lib
    for mesa_cand in [Path("/usr/lib/x86_64-linux-gnu"), Path("/usr/lib64"), Path("/usr/lib")]:
        if mesa_cand.is_dir():
            for pat in ("libEGL*", "libGL*", "libgbm*", "libglapi*", "libdrm*", "libgallium*", "libLLVM*"):
                for m_so in mesa_cand.glob(pat):
                    if m_so.is_file() or m_so.is_symlink():
                        if m_so.is_symlink():
                            tname = os.readlink(str(m_so))
                            (dest_lib / m_so.name).unlink(missing_ok=True)
                            try:
                                os.symlink(tname, str(dest_lib / m_so.name))
                            except OSError:
                                pass
                        else:
                            shutil.copy2(m_so, dest_lib / m_so.name)
                            copy_ldd_deps(m_so, dest_lib)

    # Symlink /usr/lib/x86_64-linux-gnu -> . so dynamic loader and Mesa loaders find dri/ and gbm/
    triplet_link = dest_lib / "x86_64-linux-gnu"
    if triplet_link.exists() or triplet_link.is_symlink():
        if triplet_link.is_dir() and not triplet_link.is_symlink():
            shutil.rmtree(triplet_link)
        else:
            triplet_link.unlink()
    triplet_link.symlink_to(".")

    glvnd_share = Path("/usr/share/glvnd")
    if glvnd_share.is_dir():
        shutil.copytree(glvnd_share, dest_share / "glvnd", dirs_exist_ok=True)

    glvnd_etc = Path("/etc/glvnd")
    if glvnd_etc.is_dir():
        shutil.copytree(glvnd_etc, staging_dir / "etc" / "glvnd", dirs_exist_ok=True)

    drirc_share = Path("/usr/share/drirc.d")
    if drirc_share.is_dir():
        shutil.copytree(drirc_share, dest_share / "drirc.d", dirs_exist_ok=True)

    libinput_share = Path("/usr/share/libinput")
    if libinput_share.is_dir():
        shutil.copytree(libinput_share, dest_share / "libinput", dirs_exist_ok=True)

    # 7. System Fonts
    fonts_src = PROJECT_ROOT / "assets" / "fonts"
    fonts_dst = dest_share / "fonts"
    if fonts_src.is_dir():
        for fam in ["jetbrains-mono", "inter", "liberation-sans", "liberation-serif"]:
            src_fam = fonts_src / fam
            if src_fam.is_dir():
                shutil.copytree(src_fam, fonts_dst / fam, dirs_exist_ok=True)

    # 8. Wallpapers
    wp_dst = dest_share / "wallpapers"
    wp_dst.mkdir(parents=True, exist_ok=True)
    for wp_name in ["wallpaper.jpg", "wallpaper.png"]:
        wp_src = PROJECT_ROOT / wp_name
        if wp_src.is_file():
            shutil.copy2(wp_src, wp_dst / wp_name)
            shutil.copy2(wp_src, dest_share / wp_name)

    # 9. Canonical App Bundles
    apps_dst = dest_share / "lcl" / "apps"

    # Terminal.app
    term_app_dst = apps_dst / "Terminal.app"
    term_app_dst.mkdir(parents=True, exist_ok=True)
    (term_app_dst / "assets").mkdir(parents=True, exist_ok=True)
    (term_app_dst / "bin").mkdir(parents=True, exist_ok=True)
    term_meta = PROJECT_ROOT / "src" / "apps" / "terminal" / "metadata.json"
    term_icon = PROJECT_ROOT / "src" / "apps" / "terminal" / "assets" / "icon.png"
    if not term_meta.is_file():
        raise RuntimeError(f"Missing Terminal.app metadata at {term_meta}")
    shutil.copy2(term_meta, term_app_dst / "metadata.json")
    if term_icon.is_file():
        shutil.copy2(term_icon, term_app_dst / "assets" / "icon.png")

    # UIDemo.app
    uidemo_meta = PROJECT_ROOT / "apps" / "ui_demo" / "metadata.json"
    uidemo_icon = PROJECT_ROOT / "apps" / "ui_demo" / "assets" / "icon.png"
    if not uidemo_meta.is_file() or not uidemo_icon.is_file():
        raise RuntimeError(f"Missing UIDemo.app source files in {PROJECT_ROOT / 'apps' / 'ui_demo'}")
    uidemo_app_dst = apps_dst / "UIDemo.app"
    uidemo_app_dst.mkdir(parents=True, exist_ok=True)
    (uidemo_app_dst / "assets").mkdir(parents=True, exist_ok=True)
    (uidemo_app_dst / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(uidemo_meta, uidemo_app_dst / "metadata.json")
    shutil.copy2(uidemo_icon, uidemo_app_dst / "assets" / "icon.png")
    
    ui_demo_bin = find_built_binary("lcl_ui_demo")
    if not ui_demo_bin or not ui_demo_bin.is_file():
        raise RuntimeError(f"Compiled binary 'lcl_ui_demo' not found in build directory.")
    shutil.copy2(ui_demo_bin, uidemo_app_dst / "bin" / "ui_demo")
    (uidemo_app_dst / "bin" / "ui_demo").chmod(0o755)
    copy_ldd_deps(uidemo_app_dst / "bin" / "ui_demo", dest_lib)

    # UIDemoJS.app
    uidemojs_meta = PROJECT_ROOT / "apps" / "ui_demo_js" / "metadata.json"
    uidemojs_icon = PROJECT_ROOT / "apps" / "ui_demo_js" / "assets" / "icon.png"
    uidemojs_main = PROJECT_ROOT / "apps" / "ui_demo_js" / "main.js"
    if not uidemojs_meta.is_file() or not uidemojs_icon.is_file() or not uidemojs_main.is_file():
        raise RuntimeError(f"Missing UIDemoJS.app source files in {PROJECT_ROOT / 'apps' / 'ui_demo_js'}")
    uidemojs_app_dst = apps_dst / "UIDemoJS.app"
    uidemojs_app_dst.mkdir(parents=True, exist_ok=True)
    (uidemojs_app_dst / "assets").mkdir(parents=True, exist_ok=True)
    (uidemojs_app_dst / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(uidemojs_meta, uidemojs_app_dst / "metadata.json")
    shutil.copy2(uidemojs_icon, uidemojs_app_dst / "assets" / "icon.png")
    shutil.copy2(uidemojs_main, uidemojs_app_dst / "bin" / "main.js")
    (uidemojs_app_dst / "bin" / "main.js").chmod(0o755)

    # Link /home/user/Applications to canonical /usr/share/lcl/apps
    user_apps = staging_dir / "home" / "user" / "Applications"
    for app_dir in apps_dst.iterdir():
        if app_dir.is_dir() and app_dir.name.endswith(".app"):
            dst_link = user_apps / app_dir.name
            if dst_link.exists():
                shutil.rmtree(dst_link)
            shutil.copytree(app_dir, dst_link)

    # Validate all app bundles in staging directory
    validate_app_bundles(staging_dir)

    # 10. Canonical Environment Files
    (staging_dir / "etc" / "profile").write_text(
        "export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH\n"
        "export HOME=/home/user\n"
        "export TERM=xterm-256color\n"
        "export HISTSIZE=500\n"
        "export HISTFILESIZE=1000\n"
        "alias ls='ls --color=auto'\n"
        "alias ll='ls -la'\n"
        "if [ -f /home/user/.bashrc ]; then\n"
        "    . /home/user/.bashrc\n"
        "fi\n"
    )

    (staging_dir / "home" / "user" / ".bashrc").write_text(
        "export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH\n"
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

    (staging_dir / "home" / "user" / ".profile").write_text(". /home/user/.bashrc\n")

    # 11. Canonical RootFS /init Entrypoint
    init_script_content = """#!/bin/sh
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /var/log /var/tmp /dev/pts /dev/input /home/user/Desktop /home/user/Documents /home/user/Downloads /home/user/Applications
mount -t devpts devpts /dev/pts -o mode=0620,ptmxmode=0666 2>/dev/null || mount -t devpts devpts /dev/pts 2>/dev/null || true
if [ ! -e /dev/ptmx ]; then
    mknod -m 666 /dev/ptmx c 5 2 2>/dev/null || ln -sf pts/ptmx /dev/ptmx 2>/dev/null || true
fi
chmod 666 /dev/ptmx 2>/dev/null || true

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
echo "DRM devices detected:"
ls -la /dev/dri/ 2>/dev/null || echo "  (none)"
echo "Input devices detected:"
ls /dev/input/ 2>/dev/null || echo "  (none yet)"

# Start Compositor Display Server
/bin/lcl-core 2>&1 | tee /var/log/lcl_compositor.log &
sleep 0.2

# Start LCL Session Daemon (sole application launch authority)
if [ -x /usr/bin/lcl-sessiond ]; then
    echo "[init] Starting lcl-sessiond..."
    /usr/bin/lcl-sessiond 2>&1 | tee /var/log/lcl_sessiond.log &
    sleep 0.1
fi

# Start LCL Desktop Shell
if [ -x /usr/bin/lcl-desktop-shell ]; then
    echo "[init] Starting lcl-desktop-shell..."
    /usr/bin/lcl-desktop-shell 2>&1 | tee /var/log/lcl_desktop_shell.log &
fi

wait
"""
    init_path = staging_dir / "init"
    init_path.write_text(init_script_content, encoding="utf-8")
    init_path.chmod(0o755)

    # Set permissions
    for root, dirs, files in os.walk(staging_dir):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for f in files:
            p = Path(root) / f
            if not p.is_symlink():
                # Preserve execute bit if present, else 0644
                cur = p.stat().st_mode
                if cur & stat.S_IXUSR:
                    p.chmod(0o755)
                else:
                    p.chmod(0o644)

    return sha_map


def build_rootfs_ext4(arch: str = "x86_64", image_size_mb: int = 1024) -> tuple[Path, Path, dict[str, str]]:
    """Builds the canonical ext4 rootfs image from the staging tree."""
    ROOTFS_BASE_DIR.mkdir(parents=True, exist_ok=True)
    staging_dir = ROOTFS_BASE_DIR / arch
    out_ext4 = ROOTFS_BASE_DIR / f"lcl-rootfs-{arch}.ext4"

    sha_map = stage_canonical_rootfs(staging_dir, arch)

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
    verify_rootfs_image(out_ext4)

    return staging_dir, out_ext4, sha_map


def validate_app_bundles(staging_dir: Path) -> None:
    """Strictly validates all .app bundles in the staging directory."""
    apps_dirs = [
        staging_dir / "usr" / "share" / "lcl" / "apps",
        staging_dir / "home" / "user" / "Applications",
    ]

    validated_count = 0
    for parent in apps_dirs:
        if not parent.is_dir():
            raise RuntimeError(f"App directory {parent} does not exist in staging tree.")
        for app_dir in parent.iterdir():
            if not app_dir.is_dir() or not app_dir.name.endswith(".app"):
                continue

            meta_file = app_dir / "metadata.json"
            if not meta_file.is_file():
                raise RuntimeError(f"Bundle {app_dir.name} in {parent.name} is missing metadata.json")

            try:
                meta = json.loads(meta_file.read_text(encoding="utf-8"))
            except Exception as ex:
                raise RuntimeError(f"Invalid JSON in {meta_file}: {ex}")

            app_id = meta.get("id") or meta.get("appId") or meta.get("bundleId")
            if not app_id:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'id' field in metadata.json")

            app_name = meta.get("name")
            if not app_name:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'name' field in metadata.json")

            icon_rel = meta.get("icon")
            if icon_rel and not (app_dir / icon_rel).is_file():
                raise RuntimeError(f"Bundle {app_dir.name} icon not found: {app_dir / icon_rel}")

            exec_ref = meta.get("executable") or meta.get("exec")
            if not exec_ref:
                raise RuntimeError(f"Bundle {app_dir.name} is missing 'executable' field in metadata.json")

            # Resolve executable target
            if exec_ref.startswith("/"):
                # Absolute executable path in rootfs
                target_exec = staging_dir / exec_ref.lstrip("/")
            else:
                # Bundle-relative executable path
                target_exec = app_dir / exec_ref

            if not target_exec.is_file():
                raise RuntimeError(f"Bundle {app_dir.name} resolved executable not found: {target_exec}")

            # Check script vs binary
            if exec_ref.endswith(".js"):
                # Requires lcl-js interpreter in /usr/bin/lcl-js
                interp = staging_dir / "usr" / "bin" / "lcl-js"
                if not interp.is_file():
                    raise RuntimeError(f"Bundle {app_dir.name} requires JavaScript interpreter {interp}, but not found.")
                if not os.access(str(interp), os.X_OK):
                    raise RuntimeError(f"JavaScript interpreter {interp} is not executable.")
            else:
                # Binary or shell script - check executable permission
                if not os.access(str(target_exec), os.X_OK):
                    raise RuntimeError(f"Bundle {app_dir.name} executable {target_exec} does not have execute permission (0755).")

            log(f"  ✓ Validated bundle {app_dir.name} in {parent.name} (id={app_id}, exec={exec_ref})")
            validated_count += 1

    if validated_count == 0:
        raise RuntimeError("No app bundles were found or validated in the staging tree.")


def verify_rootfs_image(ext4_path: Path) -> None:
    """Verifies that key canonical userspace files exist inside the generated ext4 image."""
    log(f"Verifying ext4 filesystem contents of {ext4_path.name}...")
    required_files = [
        "/bin/bash",
        "/bin/lcl-terminal",
        "/bin/lcl-sessiond",
        "/bin/lcl-desktop-shell",
        "/bin/lcl-open",
        "/bin/lcl-core",
        "/usr/bin/lcl-js",
        "/usr/share/lcl/apps/Terminal.app/metadata.json",
        "/usr/share/lcl/apps/UIDemo.app/metadata.json",
        "/usr/share/lcl/apps/UIDemo.app/bin/ui_demo",
        "/usr/share/lcl/apps/UIDemoJS.app/metadata.json",
        "/usr/share/lcl/apps/UIDemoJS.app/bin/main.js",
        "/home/user/Applications/Terminal.app/metadata.json",
        "/home/user/Applications/UIDemo.app/bin/ui_demo",
        "/home/user/Applications/UIDemoJS.app/bin/main.js",
        "/usr/lib/gbm/dri_gbm.so",
        "/usr/lib/dri/virtio_gpu_dri.so",
        "/etc/profile",
        "/home/user/.bashrc",
        "/init",
    ]

    for req in required_files:
        cmd = ["debugfs", "-R", f"stat {req}", str(ext4_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if "Inode:" not in res.stdout or res.returncode != 0:
            raise AssertionError(f"Required canonical file '{req}' missing from rootfs image {ext4_path}")

    # Check Terminal.app metadata content
    cat_cmd = ["debugfs", "-R", "cat /usr/share/lcl/apps/Terminal.app/metadata.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    meta_json = json.loads(cat_res.stdout)
    if meta_json.get("executable") != "/bin/lcl-terminal":
        raise AssertionError(f"Terminal.app metadata in rootfs has invalid executable: {meta_json.get('executable')}")

    # Check UIDemo.app metadata content
    cat_cmd = ["debugfs", "-R", "cat /usr/share/lcl/apps/UIDemo.app/metadata.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    meta_json = json.loads(cat_res.stdout)
    if meta_json.get("executable") != "bin/ui_demo":
        raise AssertionError(f"UIDemo.app metadata in rootfs has invalid executable: {meta_json.get('executable')}")

    # Check UIDemoJS.app metadata content
    cat_cmd = ["debugfs", "-R", "cat /usr/share/lcl/apps/UIDemoJS.app/metadata.json", str(ext4_path)]
    cat_res = subprocess.run(cat_cmd, capture_output=True, text=True, check=True)
    meta_json = json.loads(cat_res.stdout)
    if meta_json.get("executable") != "bin/main.js":
        raise AssertionError(f"UIDemoJS.app metadata in rootfs has invalid executable: {meta_json.get('executable')}")

    log("✓ All required canonical userspace files and metadata verified successfully.")


def main() -> None:
    parser = argparse.ArgumentParser(description="LCL Core Linux - Canonical RootFS Builder")
    parser.add_argument("--arch", default="x86_64", help="Target architecture (default: x86_64)")
    parser.add_argument("--size", type=int, default=1024, help="Filesystem size in MB (default: 1024)")
    args = parser.parse_args()

    staging_dir, out_ext4, sha_map = build_rootfs_ext4(arch=args.arch, image_size_mb=args.size)
    print("\n--- Summary ---")
    print(f"Staging tree: {staging_dir}")
    print(f"RootFS image: {out_ext4} ({out_ext4.stat().st_size} bytes)")
    for k, v in sha_map.items():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
