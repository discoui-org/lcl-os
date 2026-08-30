#!/usr/bin/env python3
"""
LCL Core Linux - Android Bootable Image Builder (Stage 4C.3)
Builds reproducible bootable custom system.img and ramdisk.img for Android AVD.

Stage 4C.3 Architecture:
The Android system image contains ONLY platform-specific substrate components:
- Android init, binder, vendor HALs, Composer3
- lcl-core-android (Android composition server)
- lcl.rc and lcl-bootstrap.sh init scripts

All canonical userspace applications (Terminal.app, UIDemo.app, UIDemoJS.app),
daemons, both Gestalt-selectable shells, Bash, fonts, and user home (/Users/Rei)
reside exclusively in the shared, byte-for-byte canonical rootfs:
    out/rootfs/lcl-rootfs-x86_64.ext4
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
PROJECT_ROOT = SCRIPT_DIR.parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tooling.paths import ANDROID_IMAGES_DIR, ROOTFS_DIR, android_build_dir

BUILD_ANDROID_DIR = android_build_dir("x86_64")
OUT_ANDROID_DIR = ANDROID_IMAGES_DIR
STAGE_DIR = OUT_ANDROID_DIR / "stage"
CANONICAL_ROOTFS_EXT4 = ROOTFS_DIR / "lcl-rootfs-x86_64.ext4"


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


def modify_system_image(system_ext4_path: Path, android_core: Path,
                        rasterd: Path) -> None:
    """Modifies the unpacked ext4 system.img using debugfs for platform substrate only."""
    log("Modifying system.img (configuring LCL Android substrate & init services)...")

    # Resize system.img to 1060MB to allocate sufficient inodes and space
    subprocess.run(["e2fsck", "-fy", str(system_ext4_path)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["resize2fs", str(system_ext4_path), "1060M"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Prepare temporary files for modified init scripts and LCL bootstrap
    tmp_dir = STAGE_DIR / "init_mods"
    tmp_dir.mkdir(parents=True, exist_ok=True)

    # 1. Disable surfaceflinger in surfaceflinger.rc
    res = subprocess.run(["debugfs", "-R", "cat /etc/init/surfaceflinger.rc", str(system_ext4_path)],
                         capture_output=True, text=True, check=True)
    sf_rc = res.stdout
    if "disabled" not in sf_rc:
        sf_rc = sf_rc.replace("class core animation", "class core animation\n    disabled")
    sf_rc = sf_rc.replace("onrestart", "# onrestart")
    sf_rc_path = tmp_dir / "surfaceflinger.rc"
    sf_rc_path.write_text(sf_rc)

    # 2. Disable zygote completely in init.zygote64.rc and init.zygote64_32.rc
    zygote_rc_path = tmp_dir / "init.zygote64.rc"
    zygote_rc_path.write_text("# Zygote disabled in LCL Core Linux\n")
    zygote64_32_rc_path = tmp_dir / "init.zygote64_32.rc"
    zygote64_32_rc_path.write_text("# Zygote disabled in LCL Core Linux\n")

    # 3. Disable bootanim in bootanim.rc
    res = subprocess.run(["debugfs", "-R", "cat /etc/init/bootanim.rc", str(system_ext4_path)],
                         capture_output=True, text=True, check=True)
    bootanim_rc = res.stdout
    if "disabled" not in bootanim_rc:
        bootanim_rc = bootanim_rc.replace("class core animation", "class core animation\n    disabled")
    bootanim_rc_path = tmp_dir / "bootanim.rc"
    bootanim_rc_path.write_text(bootanim_rc)

    # 4. Create LCL Android Init configuration (lcl.rc)
    lcl_rc_content = """# LCL Core Linux - Android Platform Init Script
on early-init
    mount tmpfs tmpfs /Runtime mode=0755,uid=0,gid=0

on post-fs-data
    mkdir /Runtime/Sessions 0755 root root
    mkdir /Runtime/Sessions/Rei 0700 root root
    mkdir /Runtime/Temporary 1777 root root
    chmod 0755 /Runtime
    chmod 0755 /mnt/lcl
    chmod 0755 /mnt/lcl-probe
    chmod 0755 /Runtime/Sessions
    chmod 0700 /Runtime/Sessions/Rei
    chmod 1777 /Runtime/Temporary

on boot
    start lcl-bootstrap

service lcl-bootstrap /system/bin/lcl-bootstrap.sh
    class main
    user root
    group graphics drmrpc readproc root
    seclabel u:r:su:s0
    capabilities SYS_ADMIN SYS_CHROOT SYS_NICE SETUID SETGID
"""
    lcl_rc_path = tmp_dir / "lcl.rc"
    lcl_rc_path.write_text(lcl_rc_content)

    # 5. Create empty fstab to satisfy toybox mount
    fstab_path = tmp_dir / "fstab"
    fstab_path.write_text("# LCL Core Linux fstab\n")

    # 6. Create LCL bootstrap executable script (/system/bin/lcl-bootstrap.sh)
    # Stage 4C.3: Mounts exact canonical rootfs and launches session inside canonical namespace
    lcl_bootstrap_content = """#!/system/bin/sh
# LCL Core Linux - Direct Boot Execution Script (Stage 4C.3 Canonical Userspace)

BOOT_PID=$$
CORE_PID=""
SESSION_RUNNER_PID=""

trap 'EXIT_CODE=$?; echo "[LCL BOOT] BOOTSTRAP EXIT rc=$EXIT_CODE (core_pid=$CORE_PID session_runner_pid=$SESSION_RUNNER_PID)" >> /data/local/tmp/lcl-bootstrap.log; echo "[LCL BOOT] BOOTSTRAP EXIT rc=$EXIT_CODE"' EXIT

log_boot() {
    echo "[LCL BOOT] $1"
    echo "[LCL BOOT] $1" >> /data/local/tmp/lcl-bootstrap.log
}

log_boot "=================================================="
log_boot "LCL Android Bootstrap Starting (PID=$BOOT_PID)"
log_boot "=================================================="

# 1. Setup /Runtime tmpfs
mkdir -p /Runtime 2>/dev/null
TMPFS_MOUNT_ERR=""
if ! mount | grep -q " /Runtime "; then
    TMPFS_MOUNT_ERR=$(mount -t tmpfs -o mode=0755,uid=0,gid=0 tmpfs /Runtime 2>&1)
fi

if mount | grep -q " /Runtime "; then
    mkdir -p /Runtime/Sessions/Rei /Runtime/Temporary 2>/dev/null
    chmod 0755 /Runtime 2>/dev/null
    chmod 0700 /Runtime/Sessions/Rei 2>/dev/null
    chmod 1777 /Runtime/Temporary 2>/dev/null
    rm -f /Runtime/lcl-compositor.sock /Runtime/lcl-sessiond.sock 2>/dev/null
    : > /Runtime/lcl-core.log 2>/dev/null
    : > /Runtime/lcl-sessiond.log 2>/dev/null
    : > /Runtime/lcl-shell.log 2>/dev/null
    : > /Runtime/lcl-session-runner.log 2>/dev/null
    ln -sf /Runtime/lcl-core.log /data/local/tmp/lcl-core.log 2>/dev/null
    ln -sf /Runtime/lcl-sessiond.log /data/local/tmp/lcl-sessiond.log 2>/dev/null
    ln -sf /Runtime/lcl-shell.log /data/local/tmp/lcl-shell.log 2>/dev/null
    ln -sf /Runtime/lcl-session-runner.log /data/local/tmp/lcl-session-runner.log 2>/dev/null
    if [ -f /vendor/etc/lcl/gestalt.json ]; then
        cp /vendor/etc/lcl/gestalt.json /Runtime/gestalt.json 2>/dev/null || true
        chmod 0600 /Runtime/gestalt.json 2>/dev/null || true
    fi
    log_boot "Runtime mount = /Runtime (tmpfs ready, stale sockets purged)"
else
    log_boot "ERROR: tmpfs mount on /Runtime failed: $TMPFS_MOUNT_ERR"
    sleep 2
    exit 1
fi

# 2. Ensure mountpoints /mnt/lcl and /mnt/lcl-probe exist
mkdir -p /mnt/lcl /mnt/lcl-probe 2>/dev/null
chmod 0755 /mnt/lcl /mnt/lcl-probe 2>/dev/null
umount /mnt/lcl-probe 2>/dev/null || true

log_boot "Mount namespace: $(readlink /proc/$BOOT_PID/ns/mnt 2>/dev/null || echo 'unknown')"

# 3. Check if /mnt/lcl already has a valid canonical rootfs
LCL_BLOCK=""

if [ -f /mnt/lcl/System/Core/lcl-sessiond ]; then
    log_boot "existing canonical rootfs detected on /mnt/lcl"
    log_boot "reusing /mnt/lcl"
    LCL_BLOCK=$(mount | grep " /mnt/lcl " | awk '{print $1}' | head -n 1)
    if [ -z "$LCL_BLOCK" ]; then
        LCL_BLOCK="/dev/block/vdf"
    fi
    log_boot "rootfs device = $LCL_BLOCK"
else
    log_boot "Scanning block candidates..."
    for attempt in $(seq 1 30); do
        CANDIDATES=""
        if [ -f /proc/partitions ]; then
            while read major minor blocks name; do
                case "$name" in
                    name|""|ram*|loop*|dm-*) continue ;;
                    *)
                        for prefix in /dev/block /dev; do
                            if [ -b "$prefix/$name" ]; then
                                CANDIDATES="$CANDIDATES $prefix/$name"
                            fi
                        done
                        ;;
                esac
            done < /proc/partitions
        fi

        for p in /dev/block/by-label/lcl-rootfs /dev/block/vd* /dev/vd*; do
            if [ -b "$p" ]; then
                CANDIDATES="$CANDIDATES $p"
            fi
        done

        for dev in $CANDIDATES; do
            if [ ! -b "$dev" ]; then
                continue
            fi

            log_boot "probing candidate: $dev on /mnt/lcl-probe"
            umount /mnt/lcl-probe 2>/dev/null || true
            MOUNT_ERR=$(mount -t ext4 -o ro "$dev" /mnt/lcl-probe 2>&1)
            MOUNT_RC=$?

            if [ $MOUNT_RC -eq 0 ]; then
                log_boot "  mount $dev -> SUCCESS"
                if [ -f /mnt/lcl-probe/System/Core/lcl-sessiond ]; then
                    log_boot "  signature check -> FOUND (/System/Core/lcl-sessiond present)"
                    LCL_BLOCK="$dev"
                    umount /mnt/lcl-probe 2>/dev/null || true
                    break 2
                else
                    log_boot "  signature check -> NOT LCL rootfs"
                    umount /mnt/lcl-probe 2>/dev/null || true
                fi
            else
                log_boot "  mount $dev -> FAILED (rc=$MOUNT_RC): $MOUNT_ERR"
            fi
        done

        sleep 0.2
    done

    umount /mnt/lcl-probe 2>/dev/null || true

    if [ -z "$LCL_BLOCK" ]; then
        log_boot "ERROR: canonical rootfs block device not found!"
        sleep 2
        exit 1
    fi

    log_boot "rootfs device = $LCL_BLOCK"

    FINAL_MOUNT_ERR=$(mount -t ext4 -o rw,noatime "$LCL_BLOCK" /mnt/lcl 2>&1 || mount -t ext4 "$LCL_BLOCK" /mnt/lcl 2>&1 || mount -t ext4 -o ro "$LCL_BLOCK" /mnt/lcl 2>&1)

    if [ ! -f /mnt/lcl/System/Core/lcl-sessiond ]; then
        log_boot "ERROR: Final mount failed: $FINAL_MOUNT_ERR"
        sleep 2
        exit 1
    fi

    log_boot "rootfs mounted = /mnt/lcl"
    log_boot "LCL root = /mnt/lcl"
fi

# 4. Idempotent Bind-mounts for essential kernel pseudo-filesystems and /Runtime into canonical userspace
mkdir -p /mnt/lcl/dev /mnt/lcl/dev/pts /mnt/lcl/proc /mnt/lcl/sys /mnt/lcl/Runtime 2>/dev/null

if ! mount | grep -q " /mnt/lcl/dev "; then
    mount --bind /dev /mnt/lcl/dev 2>>/data/local/tmp/lcl-bootstrap.log || true
fi
if ! mount | grep -q " /mnt/lcl/dev/pts "; then
    mount --bind /dev/pts /mnt/lcl/dev/pts 2>>/data/local/tmp/lcl-bootstrap.log || true
fi
if ! mount | grep -q " /mnt/lcl/proc "; then
    mount --bind /proc /mnt/lcl/proc 2>>/data/local/tmp/lcl-bootstrap.log || true
fi
if ! mount | grep -q " /mnt/lcl/sys "; then
    mount --bind /sys /mnt/lcl/sys 2>>/data/local/tmp/lcl-bootstrap.log || true
fi
if ! mount | grep -q " /mnt/lcl/Runtime "; then
    BIND_RUNTIME_ERR=$(mount --bind /Runtime /mnt/lcl/Runtime 2>&1)
    if [ $? -eq 0 ]; then
        log_boot "bind /Runtime -> /mnt/lcl/Runtime = SUCCESS"
    else
        log_boot "ERROR: bind /Runtime -> /mnt/lcl/Runtime failed: $BIND_RUNTIME_ERR"
        sleep 2
        exit 1
    fi
else
    log_boot "/mnt/lcl/Runtime already bound = REUSED"
fi

# 5. Start Android Platform Compositor in background
log_boot "step=core-spawn begin"
/system/bin/lcl-core-android > /Runtime/lcl-core.log 2>&1 &
CORE_PID=$!
log_boot "step=core-spawn pid=$CORE_PID"

# 6. Wait for compositor socket readiness
log_boot "step=socket-wait begin"
SOCKET_READY=0
for i in $(seq 1 100); do
    if [ -S "/Runtime/lcl-compositor.sock" ] && kill -0 $CORE_PID 2>/dev/null; then
        SOCKET_READY=1
        break
    fi
    if ! kill -0 $CORE_PID 2>/dev/null; then
        wait $CORE_PID
        CORE_RC=$?
        log_boot "ERROR: lcl-core-android exited prematurely (rc=$CORE_RC)!"
        log_boot "--- /Runtime/lcl-core.log ---"
        cat /Runtime/lcl-core.log 2>/dev/null | while read -r line; do log_boot "  $line"; done
        log_boot "-----------------------------"
        sleep 3
        exit 1
    fi
    sleep 0.05
done

if [ $SOCKET_READY -ne 1 ]; then
    log_boot "ERROR: compositor socket not ready after 5s!"
    kill -9 $CORE_PID 2>/dev/null || true
    sleep 2
    exit 1
fi

log_boot "step=socket-wait ready (core_pid=$CORE_PID)"

# 7. Test chroot viability
CHROOT_TEST=$(chroot /mnt/lcl /System/Tools/bash -c "echo CHROOT_OK" 2>&1)
log_boot "chroot bash test: $CHROOT_TEST"

# 8. Create canonical session startup script in shared /Runtime
cat << 'EOF' > /Runtime/lcl-session-start.sh
#!/System/Tools/bash
# LCL Core Linux - Canonical Session Startup Script

export PATH=/System/Core:/System/Tools
export HOME=/Users/Rei
export USER=Rei
export TERM=xterm-256color

echo "[LCL SESSION] Starting sessiond..."
/System/Core/lcl-sessiond > /Runtime/lcl-sessiond.log 2>&1 &
SESSIOND_PID=$!
echo "[LCL SESSION] lcl-sessiond spawned (PID=$SESSIOND_PID)"

# Wait for session daemon socket
for ((i=0; i<100; i++)); do
    if [ -S "/Runtime/lcl-sessiond.sock" ]; then
        echo "[LCL SESSION] lcl-sessiond.sock ready!"
        break
    fi
    if ! kill -0 $SESSIOND_PID 2>/dev/null; then
        echo "[LCL SESSION] ERROR: lcl-sessiond died prematurely!"
        break
    fi
    sleep 0.05
done

echo "[LCL SESSION] Starting Gestalt-selected shell..."
/System/Core/lcl-shell-launcher > /Runtime/lcl-shell.log 2>&1 &
SHELL_PID=$!
echo "[LCL SESSION] shell launcher spawned (PID=$SHELL_PID)"

wait $SESSIOND_PID $SHELL_PID
EOF

chmod 0755 /Runtime/lcl-session-start.sh
log_boot "step=session-script-created on /Runtime/lcl-session-start.sh"

# 9. Launch canonical session via chroot executing the script directly
log_boot "step=session-launch begin"
chroot /mnt/lcl /System/Tools/bash /Runtime/lcl-session-start.sh > /Runtime/lcl-session-runner.log 2>&1 &
SESSION_RUNNER_PID=$!
log_boot "step=session-launch pid=$SESSION_RUNNER_PID"

# 10. Robust process lifecycle monitoring loop
log_boot "step=monitor-loop begin (core_pid=$CORE_PID session_runner_pid=$SESSION_RUNNER_PID)"

while kill -0 $CORE_PID 2>/dev/null; do
    if [ -n "$SESSION_RUNNER_PID" ] && ! kill -0 $SESSION_RUNNER_PID 2>/dev/null; then
        wait $SESSION_RUNNER_PID
        SESSION_RC=$?
        log_boot "step=session-runner-exited (rc=$SESSION_RC)"
        log_boot "--- /Runtime/lcl-session-runner.log ---"
        cat /Runtime/lcl-session-runner.log 2>/dev/null | while read -r line; do log_boot "  $line"; done
        log_boot "--- /Runtime/lcl-sessiond.log ---"
        cat /Runtime/lcl-sessiond.log 2>/dev/null | while read -r line; do log_boot "  $line"; done
        log_boot "--- /Runtime/lcl-shell.log ---"
        cat /Runtime/lcl-shell.log 2>/dev/null | while read -r line; do log_boot "  $line"; done
        log_boot "-------------------------------------"
        SESSION_RUNNER_PID=""
    fi
    sleep 1
done

wait $CORE_PID
CORE_RC=$?
log_boot "step=core-exited (rc=$CORE_RC)"
log_boot "--- /Runtime/lcl-core.log ---"
cat /Runtime/lcl-core.log 2>/dev/null | while read -r line; do log_boot "  $line"; done
log_boot "-----------------------------"

sleep 5
"""
    lcl_bootstrap_path = tmp_dir / "lcl-bootstrap.sh"
    lcl_bootstrap_path.write_text(lcl_bootstrap_content)
    os.chmod(lcl_bootstrap_path, 0o755)

    # Assemble debugfs commands
    debugfs_script = []

    # Ensure /Runtime, /mnt/lcl, and /mnt/lcl-probe mountpoints exist in second-stage rootfs
    debugfs_script.extend([
        "cd /",
        "mkdir Runtime",
        "sif Runtime mode 040755",
        "mkdir mnt",
        "sif mnt mode 040755",
        "cd /mnt",
        "mkdir lcl",
        "sif lcl mode 040755",
        "mkdir lcl-probe",
        "sif lcl-probe mode 040755",
    ])

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
    debugfs_script.append(f"write {zygote_rc_path} init.zygote64_32.rc")
    debugfs_script.append("sif init.zygote64_32.rc mode 0100644")

    # Install fstab in /system/etc
    debugfs_script.append("cd /system/etc")
    debugfs_script.append("rm fstab")
    debugfs_script.append(f"write {fstab_path} fstab")
    debugfs_script.append("sif fstab mode 0100644")

    # Install the compositor, its out-of-process raster service and bootstrap.
    debugfs_script.extend([
        "cd /system/bin",
        "rm lcl-core-android",
        f"write {android_core} lcl-core-android",
        "sif lcl-core-android mode 0100755",
        "rm lcl-rasterd-android",
        f"write {rasterd} lcl-rasterd-android",
        "sif lcl-rasterd-android mode 0100755",
        "rm lcl-bootstrap.sh",
        f"write {lcl_bootstrap_path} lcl-bootstrap.sh",
        "sif lcl-bootstrap.sh mode 0100755",
        "quit",
    ])

    cmd_input = "\n".join(debugfs_script) + "\n"
    res = subprocess.run(["debugfs", "-w", str(system_ext4_path)], input=cmd_input, text=True, capture_output=True)
    if res.returncode != 0:
        err(f"debugfs execution failed:\n{res.stderr}")
        raise RuntimeError("Failed to modify ext4 system.img with debugfs")


def make_cpio_dir_entry(path_str: str, mode: int = 0o40755) -> bytes:
    name_bytes = path_str.encode("utf-8") + b"\x00"
    namesize = len(name_bytes)
    header = f"070701{1:08X}{mode:08X}{0:08X}{0:08X}{2:08X}{0:08X}{0:08X}{0:08X}{0:08X}{0:08X}{0:08X}{namesize:08X}{0:08X}".encode("ascii")
    pad = (4 - ((110 + namesize) % 4)) % 4
    return header + name_bytes + (b"\x00" * pad)


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

    # Insert rootfs directory entries for /Runtime, /mnt/lcl, and /mnt/lcl-probe before CPIO TRAILER
    trailer_idx = raw_cpio.rfind(b"070701")
    if trailer_idx != -1:
        extra_entries = make_cpio_dir_entry("Runtime", 0o40755) + make_cpio_dir_entry("mnt/lcl", 0o40755) + make_cpio_dir_entry("mnt/lcl-probe", 0o40755)
        raw_cpio = raw_cpio[:trailer_idx] + extra_entries + raw_cpio[trailer_idx:]
        log("  ✓ Injected /Runtime, /mnt/lcl, and /mnt/lcl-probe mountpoints into first_stage_ramdisk")

    proc = subprocess.run(["lz4", "-l", "-12", "--favor-decSpeed"], input=bytes(raw_cpio), capture_output=True, check=True)
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

    # 1. Ensure canonical rootfs artifact exists
    if not CANONICAL_ROOTFS_EXT4.is_file():
        log("Canonical rootfs ext4 image missing, building...")
        from build_rootfs import build_rootfs_ext4
        build_rootfs_ext4()

    rootfs_sha = get_sha256(CANONICAL_ROOTFS_EXT4)
    log(f"Consuming exact canonical rootfs artifact: {CANONICAL_ROOTFS_EXT4.name}")
    log(f"  Canonical RootFS SHA-256: {rootfs_sha}")

    # 2. Check Android composition daemon binary
    android_core = BUILD_ANDROID_DIR / "lcl-core-android"
    if not android_core.is_file():
        raise FileNotFoundError(f"Android core composition root ({android_core}) not found.")

    core_sha = get_sha256(android_core)
    log(f"  Android Core Platform Compositor SHA-256: {core_sha}")
    rasterd = BUILD_ANDROID_DIR / "lcl-rasterd-android"
    if not rasterd.is_file():
        raise FileNotFoundError(f"Android raster service ({rasterd}) not found.")
    log(f"  Android Raster Service SHA-256: {get_sha256(rasterd)}")

    # 3. Parse GPT partitions dynamically
    partitions = parse_gpt_partitions(paths.stock_system)
    super_part = next((p for p in partitions if p["name"] == "super"), None)
    vbmeta_part = next((p for p in partitions if p["name"] == "vbmeta"), None)
    if not super_part:
        raise ValueError("Could not find 'super' partition in stock system.img")

    log(f"Found dynamic 'super' partition at offset {super_part['offset']} (size: 1895825408 bytes)")

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
    modify_system_image(system_img, android_core, rasterd)

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
