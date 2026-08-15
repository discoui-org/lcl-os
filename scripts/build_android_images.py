#!/usr/bin/env python3
"""
LCL Core Linux - Android Bootable Image Builder (Stage 4A)
Builds reproducible bootable custom system.img and ramdisk.img for Android AVD.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_ROOT / "build"
BUILD_ANDROID_DIR = PROJECT_ROOT / "build-android"
OUT_ANDROID_DIR = BUILD_DIR / "android"
STAGE_DIR = OUT_ANDROID_DIR / "stage"


def log(msg: str) -> None:
    print(f"[LCL IMG] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[LCL IMG ERROR] {msg}", file=sys.stderr, flush=True)


def get_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


class AndroidSdkPaths:
    def __init__(self) -> None:
        sdk_candidates = [
            os.environ.get("ANDROID_SDK_ROOT"),
            os.environ.get("ANDROID_HOME"),
            Path.home() / "Android/Sdk",
            Path.home() / ".android/sdk",
            Path("/opt/android-sdk"),
        ]
        self.sdk_root: Path | None = None
        for c in sdk_candidates:
            if c and Path(c).is_dir():
                self.sdk_root = Path(c)
                break

        if not self.sdk_root:
            raise FileNotFoundError("Android SDK not found. Set ANDROID_SDK_ROOT.")

        self.sys_img_dir = self.sdk_root / "system-images/android-36/default/x86_64"
        if not self.sys_img_dir.is_dir():
            raise FileNotFoundError(f"Android 36 x86_64 system image directory not found at {self.sys_img_dir}")

        self.stock_system = self.sys_img_dir / "system.img"
        self.stock_vendor = self.sys_img_dir / "vendor.img"
        self.stock_kernel = self.sys_img_dir / "kernel-ranchu"

        # Check for clean stock ramdisk backup, else ramdisk.img
        if (self.sys_img_dir / "ramdisk.img.backup").is_file():
            self.stock_ramdisk = self.sys_img_dir / "ramdisk.img.backup"
        else:
            self.stock_ramdisk = self.sys_img_dir / "ramdisk.img"

        # Host tools
        for tool in ["lpunpack", "lpmake", "simg2img", "debugfs", "lz4", "cpio"]:
            if not shutil.which(tool):
                raise FileNotFoundError(f"Required host tool '{tool}' not found in PATH.")


def parse_gpt_partitions(disk_path: Path) -> list[dict]:
    """Dynamically parses GPT partition table without hardcoded sector offsets."""
    partitions = []
    with open(disk_path, "rb") as f:
        # LBA 1 (GPT Header)
        f.seek(512)
        gpt_head = f.read(512)
        if gpt_head[:8] != b"EFI PART":
            raise ValueError(f"Invalid GPT header in {disk_path}")

        (
            sig, rev, size, crc, rsvd, cur_lba, bkp_lba,
            first_usable, last_usable, disk_guid,
            part_lba, num_parts, part_size, part_crc
        ) = struct.unpack("<8sIIIIQQQQ16sQIII", gpt_head[:92])

        f.seek(part_lba * 512)
        for i in range(num_parts):
            entry = f.read(part_size)
            if len(entry) < 128:
                break
            type_guid = entry[:16]
            if type_guid == b"\x00" * 16:
                continue
            first_lba, last_lba, flags = struct.unpack("<QQQ", entry[32:56])
            name = entry[56:128].decode("utf-16le").strip("\x00")
            offset = first_lba * 512
            length = (last_lba - first_lba + 1) * 512
            partitions.append({
                "index": i + 1,
                "name": name,
                "first_lba": first_lba,
                "last_lba": last_lba,
                "offset": offset,
                "size": length,
            })
    return partitions


def modify_system_image(system_ext4_path: Path, glibc_runtime_dir: Path) -> None:
    """Modifies the unpacked ext4 system.img using debugfs."""
    log("Modifying system.img (disabling Android UI, installing LCL userspace & canonical assets)...")

    # 0. Resize system.img to 1060MB to allocate sufficient inodes (new block group) and blocks for LCL runtime and assets
    subprocess.run(["e2fsck", "-fy", str(system_ext4_path)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["resize2fs", str(system_ext4_path), "1060M"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 1. Prepare temporary files for modified init scripts and LCL bootstrap
    tmp_dir = STAGE_DIR / "init_mods"
    tmp_dir.mkdir(parents=True, exist_ok=True)

    # Disable surfaceflinger in surfaceflinger.rc
    res = subprocess.run(["debugfs", "-R", "cat /etc/init/surfaceflinger.rc", str(system_ext4_path)],
                         capture_output=True, text=True, check=True)
    sf_rc = res.stdout
    if "disabled" not in sf_rc:
        sf_rc = sf_rc.replace("class core animation", "class core animation\n    disabled")
    sf_rc = sf_rc.replace("onrestart", "# onrestart")
    sf_rc_path = tmp_dir / "surfaceflinger.rc"
    sf_rc_path.write_text(sf_rc)

    # Disable zygote completely in init.zygote64.rc and init.zygote64_32.rc
    zygote_rc_path = tmp_dir / "init.zygote64.rc"
    zygote_rc_path.write_text("# Zygote disabled in LCL Core Linux\n")
    zygote64_32_rc_path = tmp_dir / "init.zygote64_32.rc"
    zygote64_32_rc_path.write_text("# Zygote disabled in LCL Core Linux\n")

    # Disable bootanim in bootanim.rc
    res = subprocess.run(["debugfs", "-R", "cat /etc/init/bootanim.rc", str(system_ext4_path)],
                         capture_output=True, text=True, check=True)
    bootanim_rc = res.stdout
    if "disabled" not in bootanim_rc:
        bootanim_rc = bootanim_rc.replace("class core animation", "class core animation\n    disabled")
    bootanim_rc_path = tmp_dir / "bootanim.rc"
    bootanim_rc_path.write_text(bootanim_rc)

    # Create LCL Android Init configuration
    lcl_rc_content = """# LCL Core Linux - Android Platform Init Script
on post-fs-data
    mount tmpfs tmpfs /run nodev noexec nosuid mode=0755,uid=0,gid=0
    mkdir /run/user 0755 root root
    mkdir /run/user/1000 0755 root root
    mkdir /run/user/0 0755 root root
    chmod 0755 /run
    chmod 0755 /run/user
    chmod 0755 /run/user/1000
    chmod 0755 /run/user/0

on boot
    start lcl-bootstrap

service lcl-bootstrap /system/lcl/bin/lcl-bootstrap.sh
    class main
    user root
    group graphics drmrpc readproc root
    seclabel u:r:su:s0
    capabilities SYS_NICE
"""
    lcl_rc_path = tmp_dir / "lcl.rc"
    lcl_rc_path.write_text(lcl_rc_content)

    # Create LCL bootstrap executable script
    lcl_bootstrap_content = """#!/system/bin/sh
# LCL Core Linux - Direct Boot Execution Script

# 1. Mount writable /run tmpfs if not mounted yet
if ! mount | grep -q " /run "; then
    mount -t tmpfs -o mode=0755,uid=0,gid=0 tmpfs /run 2>/dev/null
fi
mkdir -p /run/user/1000 /run/user/0 2>/dev/null
chmod 755 /run /run/user /run/user/1000 2>/dev/null

    # 2. Canonical symlinks & home directory
[ ! -e /usr ] && ln -s /system/lcl/usr /usr 2>/dev/null
[ ! -e /bin ] && ln -s /system/lcl/bin /bin 2>/dev/null
[ ! -e /lib64 ] && ln -s /system/lcl/lib64 /lib64 2>/dev/null
[ ! -e /home ] && ln -s /system/lcl/home /home 2>/dev/null
mkdir -p /home/user/Applications /home/user/Desktop /home/user/Documents /home/user/Downloads 2>/dev/null

# 3. Export canonical environment
export PATH="/usr/bin:/bin:/system/lcl/bin:/system/bin:$PATH"
export HOME="/home/user"
export LD_LIBRARY_PATH="/system/lib64:/apex/com.android.i18n/lib64:/apex/com.android.art/lib64:/vendor/lib64:/vendor/lib64/hw:/system/lib64/hw:/system/lcl/lib64"
export LCL_COMPOSITOR_SOCKET="/run/user/1000/lcl-compositor.sock"
export LCL_SESSION_SOCKET="/run/user/1000/lcl-sessiond.sock"

# 4. Start LCL Platform Compositor directly on Composer3
/system/lcl/bin/lcl-core-android > /data/local/tmp/lcl-core.log 2>&1 &
CORE_PID=$!

# 5. Wait for compositor socket readiness
for i in $(seq 1 100); do
    if [ -S "$LCL_COMPOSITOR_SOCKET" ]; then
        break
    fi
    sleep 0.05
done

# 6. Start LCL Session Daemon (lcl-sessiond)
/system/lcl/lib64/ld-linux-x86-64.so.2 --library-path /system/lcl/lib64 /system/lcl/bin/lcl-sessiond > /data/local/tmp/lcl-sessiond.log 2>&1 &
SESSIOND_PID=$!

# 7. Wait for session socket readiness
for i in $(seq 1 100); do
    if [ -S "/run/user/1000/lcl-sessiond.sock" ]; then
        break
    fi
    sleep 0.05
done

# 8. Launch canonical desktop shell via dynamic loader
# lcl-terminal is NOT launched here — lcl-sessiond's launchDefaultProfile() is the sole terminal launch authority
/system/lcl/lib64/ld-linux-x86-64.so.2 --library-path /system/lcl/lib64 /system/lcl/bin/lcl-desktop-shell > /data/local/tmp/lcl-shell.log 2>&1 &

# 9. Keep bootstrap process alive so init does not kill children
wait $CORE_PID $SESSIOND_PID
"""
    lcl_bootstrap_path = tmp_dir / "lcl-bootstrap.sh"
    lcl_bootstrap_path.write_text(lcl_bootstrap_content)
    os.chmod(lcl_bootstrap_path, 0o755)

    # Binaries and assets
    desktop_shell = BUILD_DIR / "lcl-desktop-shell"
    desktop_term = BUILD_DIR / "lcl-terminal"
    sessiond_bin = BUILD_DIR / "lcl-sessiond"
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"
    bash_bin = Path("/bin/bash") if Path("/bin/bash").is_file() else Path("/usr/bin/bash")
    wallpaper_jpg = PROJECT_ROOT / "wallpaper.jpg"
    wallpaper_png = PROJECT_ROOT / "wallpaper.png"

    # Canonical Terminal.app metadata (identical on all platforms)
    term_meta = PROJECT_ROOT / "src/apps/terminal/metadata.json"
    term_icon = PROJECT_ROOT / "src/apps/terminal/assets/icon.png"
    uidemo_meta = PROJECT_ROOT / "apps/ui_demo/metadata.json"
    uidemo_icon = PROJECT_ROOT / "apps/ui_demo/assets/icon.png"
    uidemojs_meta = PROJECT_ROOT / "apps/ui_demo_js/metadata.json"
    uidemojs_icon = PROJECT_ROOT / "apps/ui_demo_js/assets/icon.png"

    # Shell profile and bashrc for canonical /home/user environment
    bashrc_path = tmp_dir / "bashrc"
    bashrc_path.write_text("""export PATH=/usr/bin:/bin:/system/lcl/bin:/system/bin:$PATH
export TERM=xterm-256color
export PS1='\\[\\033[1;34m\\]\\W\\[\\033[0m\\] ❯ '
export HISTSIZE=500
alias ls='ls --color=auto'
alias ll='ls -la'
""")
    profile_path = tmp_dir / "profile"
    profile_path.write_text("""export PATH=/usr/bin:/bin:/system/lcl/bin:/system/bin:$PATH
export HOME=/home/user
export TERM=xterm-256color
if [ -f /home/user/.bashrc ]; then
    . /home/user/.bashrc
fi
""")

    # Assemble debugfs commands
    debugfs_script = []

    # Write modified .rc files
    debugfs_script.append("cd /system/etc/init")
    debugfs_script.append("rm surfaceflinger.rc")
    debugfs_script.append(f"write {sf_rc_path} surfaceflinger.rc")
    debugfs_script.append("rm bootanim.rc")
    debugfs_script.append(f"write {bootanim_rc_path} bootanim.rc")
    debugfs_script.append("rm lcl.rc")
    debugfs_script.append(f"write {lcl_rc_path} lcl.rc")
    debugfs_script.append("sif surfaceflinger.rc mode 0100644")
    debugfs_script.append("sif bootanim.rc mode 0100644")
    debugfs_script.append("sif lcl.rc mode 0100644")

    debugfs_script.append("cd /system/etc/init/hw")
    debugfs_script.append("rm init.zygote64.rc")
    debugfs_script.append(f"write {zygote_rc_path} init.zygote64.rc")
    debugfs_script.append("sif init.zygote64.rc mode 0100644")
    debugfs_script.append("rm init.zygote64_32.rc")
    debugfs_script.append(f"write {zygote64_32_rc_path} init.zygote64_32.rc")
    debugfs_script.append("sif init.zygote64_32.rc mode 0100644")

    # Create /system/lcl directories
    debugfs_script.extend([
        "cd /system",
        "mkdir lcl",
        "cd /system/lcl",
        "mkdir bin",
        "mkdir lib64",
        "mkdir etc",
        "mkdir home",
        "mkdir usr",
        "cd /system/lcl/home",
        "mkdir user",
        "cd /system/lcl/home/user",
        "mkdir Applications",
        "mkdir Desktop",
        "mkdir Documents",
        "mkdir Downloads",
        f"write {bashrc_path} .bashrc",
        "sif .bashrc mode 0100644",
        f"write {profile_path} .profile",
        "sif .profile mode 0100644",
        "cd /system/lcl/etc",
        f"write {profile_path} profile",
        "sif profile mode 0100644",
        "cd /system/lcl/usr",
        "mkdir bin",
        "mkdir share",
        "cd /system/lcl/usr/share",
        "mkdir fonts",
        "mkdir wallpapers",
        "mkdir lcl",
        "cd /system/lcl/usr/share/lcl",
        "mkdir apps",
        "cd /system/lcl/usr/share/fonts",
        "mkdir jetbrains-mono",
        "mkdir inter",
        "mkdir liberation-sans",
        "mkdir liberation-serif",
    ])

    # Install binaries into /system/lcl/bin
    debugfs_script.extend([
        "cd /system/lcl/bin",
        "rm lcl-core-android",
        f"write {android_core} lcl-core-android",
        "sif lcl-core-android mode 0100755",
        "rm lcl-sessiond",
        f"write {sessiond_bin} lcl-sessiond",
        "sif lcl-sessiond mode 0100755",
        "rm lcl-desktop-shell",
        f"write {desktop_shell} lcl-desktop-shell",
        "sif lcl-desktop-shell mode 0100755",
        "rm lcl-terminal",
        f"write {desktop_term} lcl-terminal",
        "sif lcl-terminal mode 0100755",
        "rm bash",
        f"write {bash_bin} bash",
        "sif bash mode 0100755",
        "rm lcl-bootstrap.sh",
        f"write {lcl_bootstrap_path} lcl-bootstrap.sh",
        "sif lcl-bootstrap.sh mode 0100755",
    ])

    # Install glibc runtime into /system/lcl/lib64
    debugfs_script.append("cd /system/lcl/lib64")
    runtime_lib_dir = glibc_runtime_dir / "lib" if (glibc_runtime_dir / "lib").is_dir() else glibc_runtime_dir
    seen_so = set()
    for so_file in runtime_lib_dir.glob("*.so*"):
        if so_file.is_file() and so_file.name not in seen_so:
            seen_so.add(so_file.name)
            debugfs_script.append(f"rm {so_file.name}")
            debugfs_script.append(f"write {so_file} {so_file.name}")
            debugfs_script.append(f"sif {so_file.name} mode 0100755")

    # Install canonical .app bundles into /system/lcl/usr/share/lcl/apps
    # 1. Terminal.app (canonical metadata.json pointing to /bin/lcl-terminal)
    debugfs_script.extend([
        "cd /system/lcl/usr/share/lcl/apps",
        "mkdir Terminal.app",
        "cd /system/lcl/usr/share/lcl/apps/Terminal.app",
        "mkdir assets",
        "mkdir bin",
        f"write {term_meta} metadata.json",
        "sif metadata.json mode 0100644",
        "cd /system/lcl/usr/share/lcl/apps/Terminal.app/assets",
        f"write {term_icon} icon.png",
        "sif icon.png mode 0100644",
        "cd /system/lcl/usr/share/lcl/apps/Terminal.app/bin",
        "symlink terminal /system/lcl/bin/lcl-terminal",
    ])

    # 2. UIDemo.app
    if uidemo_meta.is_file() and uidemo_icon.is_file():
        debugfs_script.extend([
            "cd /system/lcl/usr/share/lcl/apps",
            "mkdir UIDemo.app",
            "cd /system/lcl/usr/share/lcl/apps/UIDemo.app",
            "mkdir assets",
            "mkdir bin",
            f"write {uidemo_meta} metadata.json",
            "sif metadata.json mode 0100644",
            "cd /system/lcl/usr/share/lcl/apps/UIDemo.app/assets",
            f"write {uidemo_icon} icon.png",
            "sif icon.png mode 0100644",
        ])

    # 3. UIDemoJS.app
    if uidemojs_meta.is_file() and uidemojs_icon.is_file():
        debugfs_script.extend([
            "cd /system/lcl/usr/share/lcl/apps",
            "mkdir UIDemoJS.app",
            "cd /system/lcl/usr/share/lcl/apps/UIDemoJS.app",
            "mkdir assets",
            "mkdir bin",
            f"write {uidemojs_meta} metadata.json",
            "sif metadata.json mode 0100644",
            "cd /system/lcl/usr/share/lcl/apps/UIDemoJS.app/assets",
            f"write {uidemojs_icon} icon.png",
            "sif icon.png mode 0100644",
        ])

    # Install binary symlinks in /system/bin and /system/lcl/usr/bin
    debugfs_script.extend([
        "cd /system/lcl/usr/bin",
        "symlink lcl-terminal /system/lcl/bin/lcl-terminal",
        "symlink lcl-sessiond /system/lcl/bin/lcl-sessiond",
        "symlink lcl-desktop-shell /system/lcl/bin/lcl-desktop-shell",
        "symlink bash /system/lcl/bin/bash",
        "cd /system/bin",
        "symlink lcl-terminal /system/lcl/bin/lcl-terminal",
        "symlink lcl-sessiond /system/lcl/bin/lcl-sessiond",
        "symlink lcl-desktop-shell /system/lcl/bin/lcl-desktop-shell",
        "symlink bash /system/lcl/bin/bash",
    ])

    # Install fonts into canonical paths
    fonts_root = PROJECT_ROOT / "assets/fonts"
    for fam_dir in ["jetbrains-mono", "inter", "liberation-sans", "liberation-serif"]:
        src_fam = fonts_root / fam_dir
        if src_fam.is_dir():
            debugfs_script.append(f"cd /system/lcl/usr/share/fonts/{fam_dir}")
            for font_file in src_fam.glob("*.[to]tf"):
                debugfs_script.append(f"write {font_file} {font_file.name}")
                debugfs_script.append(f"sif {font_file.name} mode 0100644")

    # Install wallpapers
    debugfs_script.append("cd /system/lcl/usr/share/wallpapers")
    if wallpaper_jpg.is_file():
        debugfs_script.append(f"write {wallpaper_jpg} wallpaper.jpg")
        debugfs_script.append("sif wallpaper.jpg mode 0100644")
    if wallpaper_png.is_file():
        debugfs_script.append(f"write {wallpaper_png} wallpaper.png")
        debugfs_script.append("sif wallpaper.png mode 0100644")

    debugfs_script.append("cd /system/lcl/usr/share")
    if wallpaper_jpg.is_file():
        debugfs_script.append(f"write {wallpaper_jpg} wallpaper.jpg")
        debugfs_script.append("sif wallpaper.jpg mode 0100644")

    # Create root symlinks at /
    debugfs_script.extend([
        "cd /",
        "symlink usr system/lcl/usr",
        "symlink bin system/lcl/bin",
        "symlink lib64 system/lcl/lib64",
        "symlink home system/lcl/home",
        "mkdir run",
        "quit",
    ])

    cmd_input = "\n".join(debugfs_script) + "\n"
    res = subprocess.run(["debugfs", "-w", str(system_ext4_path)], input=cmd_input, text=True, capture_output=True)
    if res.returncode != 0:
        err(f"debugfs execution failed:\n{res.stderr}")
        raise RuntimeError("Failed to modify ext4 system.img with debugfs")


def build_custom_ramdisk(stock_ramdisk_path: Path, out_ramdisk_path: Path) -> None:
    raw_cpio = bytearray(subprocess.run(["lz4", "-d", "-c", str(stock_ramdisk_path)], check=True, capture_output=True).stdout)

    target_erofs = b"system   /system     erofs   ro               wait,logical,avb=vbmeta,first_stage_mount"
    target_ext4 = b"system   /system     ext4    ro,barrier=1     wait,logical,avb=vbmeta,first_stage_mount"

    repl_erofs = b"system   /system     erofs   ro               wait,logical,first_stage_mount           "
    repl_ext4 = b"system   /system     ext4    ro,barrier=1     wait,logical,first_stage_mount           "

    idx1 = raw_cpio.find(target_erofs)
    idx2 = raw_cpio.find(target_ext4)

    if idx1 == -1 or idx2 == -1:
        raise ValueError("Could not find stock /system fstab entries in ramdisk CPIO archive")

    raw_cpio[idx1:idx1+len(target_erofs)] = repl_erofs
    raw_cpio[idx2:idx2+len(target_ext4)] = repl_ext4
    log("  ✓ In-place patched first_stage_ramdisk/fstab.ranchu (removed avb=vbmeta for /system only)")

    proc = subprocess.run(["lz4", "-l", "-12", "--favor-decSpeed"], input=raw_cpio, capture_output=True, check=True)
    out_ramdisk_path.write_bytes(proc.stdout)
    log(f"  ✓ Packed custom ramdisk {out_ramdisk_path.name} ({out_ramdisk_path.stat().st_size} bytes)")


def build_android_images() -> tuple[Path, Path]:
    log("====================================================")
    log("  LCL Core Linux — Android Bootable Image Builder   ")
    log("====================================================")

    paths = AndroidSdkPaths()
    OUT_ANDROID_DIR.mkdir(parents=True, exist_ok=True)
    STAGE_DIR.mkdir(parents=True, exist_ok=True)

    out_system_img = OUT_ANDROID_DIR / "lcl-system.img"
    out_ramdisk_img = OUT_ANDROID_DIR / "lcl-ramdisk.img"

    # 1. Check canonical same-binary executables
    desktop_shell = BUILD_DIR / "lcl-desktop-shell"
    desktop_term = BUILD_DIR / "lcl-terminal"
    sessiond_bin = BUILD_DIR / "lcl-sessiond"
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"

    if not desktop_shell.is_file() or not desktop_term.is_file() or not sessiond_bin.is_file():
        raise FileNotFoundError("Canonical desktop applications/tools (lcl-desktop-shell, lcl-terminal, lcl-sessiond) not found.")
    if not android_core.is_file():
        raise FileNotFoundError("Android core composition root (lcl-core-android) not found.")

    shell_sha = get_sha256(desktop_shell)
    term_sha = get_sha256(desktop_term)
    sessiond_sha = get_sha256(sessiond_bin)
    log(f"Canonical Desktop Shell SHA-256:    {shell_sha}")
    log(f"Canonical Desktop Terminal SHA-256: {term_sha}")
    log(f"Canonical Session Daemon SHA-256:   {sessiond_sha}")

    # 2. Stage glibc userspace runtime
    from run_avd import stage_glibc_runtime
    runtime_dir = BUILD_DIR / "lcl-runtime"
    bash_bin = Path("/bin/bash") if Path("/bin/bash").is_file() else Path("/usr/bin/bash")
    stage_glibc_runtime([desktop_shell, desktop_term, sessiond_bin, bash_bin], runtime_dir)

    # 3. Parse GPT partitions dynamically
    partitions = parse_gpt_partitions(paths.stock_system)
    super_part = next((p for p in partitions if p["name"] == "super"), None)
    vbmeta_part = next((p for p in partitions if p["name"] == "vbmeta"), None)
    if not super_part:
        raise ValueError("Could not find 'super' partition in stock system.img")

    log(f"Found dynamic 'super' partition at offset {super_part['offset']} (size: {super_part['size']} bytes)")

    # 4. Extract dynamic super partition
    super_img_path = STAGE_DIR / "super.img"
    with open(paths.stock_system, "rb") as f_in, open(super_img_path, "wb") as f_out:
        f_in.seek(super_part["offset"])
        remaining = super_part["size"]
        while remaining > 0:
            chunk = f_in.read(min(remaining, 16 * 1024 * 1024))
            if not chunk:
                break
            f_out.write(chunk)
            remaining -= len(chunk)

    # 5. Unpack super partition using lpunpack
    unpacked_dir = STAGE_DIR / "unpacked_super"
    if unpacked_dir.exists():
        shutil.rmtree(unpacked_dir)
    unpacked_dir.mkdir(parents=True, exist_ok=True)

    log("Unpacking dynamic partitions with lpunpack...")
    subprocess.run(["lpunpack", str(super_img_path), str(unpacked_dir)], check=True, capture_output=True)

    # 6. Modify ONLY system.img (vendor.img, product.img, system_ext.img, system_dlkm.img stay 100% stock)
    system_img = unpacked_dir / "system.img"
    modify_system_image(system_img, runtime_dir)

    # 7. Rebuild dynamic super partition with lpmake
    log("Rebuilding dynamic super partition with lpmake...")
    rebuilt_sparse_super = STAGE_DIR / "super_rebuilt_sparse.img"
    rebuilt_raw_super = STAGE_DIR / "super_rebuilt_raw.img"

    system_size = os.path.getsize(system_img)
    sys_dlkm_size = os.path.getsize(unpacked_dir / "system_dlkm.img")
    sys_ext_size = os.path.getsize(unpacked_dir / "system_ext.img")
    product_size = os.path.getsize(unpacked_dir / "product.img")
    vendor_size = os.path.getsize(unpacked_dir / "vendor.img")

    lpmake_cmd = [
        "lpmake",
        "--metadata-size", "65536",
        "--super-name", "super",
        "--metadata-slots", "2",
        "--device", f"super:{super_part['size']}",
        "--group", f"emulator_dynamic_partitions:{super_part['size'] - 8388608}",
        "--partition", f"system:readonly:{system_size}:emulator_dynamic_partitions",
        "--image", f"system={system_img}",
        "--partition", f"system_dlkm:readonly:{sys_dlkm_size}:emulator_dynamic_partitions",
        "--image", f"system_dlkm={unpacked_dir / 'system_dlkm.img'}",
        "--partition", f"system_ext:readonly:{sys_ext_size}:emulator_dynamic_partitions",
        "--image", f"system_ext={unpacked_dir / 'system_ext.img'}",
        "--partition", f"product:readonly:{product_size}:emulator_dynamic_partitions",
        "--image", f"product={unpacked_dir / 'product.img'}",
        "--partition", f"vendor:readonly:{vendor_size}:emulator_dynamic_partitions",
        "--image", f"vendor={unpacked_dir / 'vendor.img'}",
        "--sparse",
        "--output", str(rebuilt_sparse_super),
    ]
    subprocess.run(lpmake_cmd, check=True, capture_output=True)

    # Convert sparse super to raw
    subprocess.run(["simg2img", str(rebuilt_sparse_super), str(rebuilt_raw_super)], check=True, capture_output=True)

    # 8. Assemble full GPT disk image (lcl-system.img) preserving primary and backup GPT tables
    log(f"Assembling {out_system_img.name}...")
    shutil.copyfile(paths.stock_system, out_system_img)

    with open(out_system_img, "r+b") as f_out:
        # Stock vbmeta on partition 1 is kept 100% intact (genuine cryptographic signature & digest).
        # Write rebuilt dynamic super partition at super offset (partition 2).
        f_out.seek(super_part["offset"])
        with open(rebuilt_raw_super, "rb") as f_super:
            shutil.copyfileobj(f_super, f_out)

    # 9. Pack custom ramdisk (lcl-ramdisk.img) with patched first_stage_ramdisk/fstab.ranchu
    log(f"Assembling {out_ramdisk_img.name}...")
    build_custom_ramdisk(paths.stock_ramdisk, out_ramdisk_img)

    # 10. Link into default AVD folder if present
    avd_dir = Path.home() / ".android/avd/lcl-phone.avd"
    if avd_dir.is_dir():
        avd_sys = avd_dir / "system.img"
        avd_ram = avd_dir / "ramdisk.img"
        try:
            avd_sys.unlink(missing_ok=True)
            avd_ram.unlink(missing_ok=True)
            avd_sys.symlink_to(out_system_img)
            avd_ram.symlink_to(out_ramdisk_img)
            log(f"  ✓ Linked images to {avd_dir}")
        except Exception as ex:
            log(f"  ! Note: Could not link to AVD dir: {ex}")

    return out_system_img, out_ramdisk_img


def main() -> None:
    parser = argparse.ArgumentParser(description="Build LCL Android Bootable Images")
    args = parser.parse_args()
    build_android_images()


if __name__ == "__main__":
    main()
