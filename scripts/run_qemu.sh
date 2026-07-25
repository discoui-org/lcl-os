#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/lcl-core"
INITRAMFS_DIR="${BUILD_DIR}/initramfs_root"
INITRAMFS_IMG="${BUILD_DIR}/initramfs.cpio.gz"

echo "===================================================="
echo "  LCL Core Linux - QEMU Isolated Boot Launcher      "
echo "===================================================="

# 1. Build LCL Core binary if missing
if [ ! -f "${BINARY}" ]; then
    echo "[LCL QEMU] Building lcl-core binary..."
    cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DCMAKE_BUILD_TYPE=Debug
    cmake --build "${BUILD_DIR}"
fi

QEMU_BIN=$(which qemu-system-x86_64 2>/dev/null || true)
if [ -z "${QEMU_BIN}" ]; then
    echo "[LCL QEMU ERROR] qemu-system-x86_64 is not installed."
    exit 1
fi

# 2. Locate Kernel Image
KERNEL_PATH=""
UNAME_R=$(uname -r 2>/dev/null || true)
POSSIBLE_KERNELS=(
    "/lib/modules/${UNAME_R}/vmlinuz"
    "/boot/vmlinuz-${UNAME_R}"
    "/boot/vmlinuz-linux"
    "/lib/modules/7.1.4-1-cachyos/vmlinuz"
)

for k in "${POSSIBLE_KERNELS[@]}"; do
    if [ -f "$k" ]; then
        KERNEL_PATH="$k"
        break
    fi
done

if [ -z "${KERNEL_PATH}" ]; then
    KERNEL_PATH=$(find /lib/modules /boot -name "vmlinuz*" 2>/dev/null | head -n 1 || true)
fi

echo "[LCL QEMU] QEMU binary: ${QEMU_BIN}"
echo "[LCL QEMU] Host Kernel: ${KERNEL_PATH}"

# 3. Create Initramfs Root with Arch/Linux Symlinks & Dynamic Libraries
echo "[LCL QEMU] Preparing initramfs root directory structure..."
mkdir -p "${INITRAMFS_DIR}"/{proc,sys,dev,tmp,etc,usr/bin,usr/lib,usr/share,home/user/Desktop,home/user/Documents,home/user/Downloads,home/user/Applications}
[ ! -L "${INITRAMFS_DIR}/bin" ] && ln -s usr/bin "${INITRAMFS_DIR}/bin"
[ ! -L "${INITRAMFS_DIR}/sbin" ] && ln -s usr/bin "${INITRAMFS_DIR}/sbin"
[ ! -L "${INITRAMFS_DIR}/lib" ] && ln -s usr/lib "${INITRAMFS_DIR}/lib"
[ ! -L "${INITRAMFS_DIR}/lib64" ] && ln -s usr/lib "${INITRAMFS_DIR}/lib64"

# Copy lcl-core, sh, and essential init utilities
cp "${BINARY}" "${INITRAMFS_DIR}/usr/bin/lcl-core"
[ -f /bin/sh ] && cp -L /bin/sh "${INITRAMFS_DIR}/usr/bin/sh"
[ -f /bin/mount ] && cp -L /bin/mount "${INITRAMFS_DIR}/usr/bin/mount"
[ -f /bin/mkdir ] && cp -L /bin/mkdir "${INITRAMFS_DIR}/usr/bin/mkdir"
[ -f /bin/sleep ] && cp -L /bin/sleep "${INITRAMFS_DIR}/usr/bin/sleep"
[ -f /bin/ls ] && cp -L /bin/ls "${INITRAMFS_DIR}/usr/bin/ls"
[ -f /usr/bin/printf ] && cp -L /usr/bin/printf "${INITRAMFS_DIR}/usr/bin/printf"
[ -f /sbin/modprobe ] && cp -L /sbin/modprobe "${INITRAMFS_DIR}/usr/bin/modprobe"
[ -d /usr/share/libinput ] && cp -r /usr/share/libinput "${INITRAMFS_DIR}/usr/share/" 2>/dev/null || true

# Copy system font assets into /usr/share/fonts/
mkdir -p "${INITRAMFS_DIR}/usr/share/fonts"
if [ -d "${ROOT_DIR}/assets/fonts" ]; then
    cp -r "${ROOT_DIR}/assets/fonts/"* "${INITRAMFS_DIR}/usr/share/fonts/" 2>/dev/null || true
fi

# Create /usr/bin/clear helper script
cat << 'EOF' > "${INITRAMFS_DIR}/usr/bin/clear"
#!/bin/sh
printf "\033[2J\033[H"
EOF
chmod +x "${INITRAMFS_DIR}/usr/bin/clear"

# Copy native C++ lcl-open tool as /usr/bin/open
OPEN_BIN="${BUILD_DIR}/lcl-open"
if [ -f "${OPEN_BIN}" ]; then
    cp "${OPEN_BIN}" "${INITRAMFS_DIR}/usr/bin/open"
else
    cat << 'EOF' > "${INITRAMFS_DIR}/usr/bin/open"
#!/bin/sh
echo "Usage: open <app_name.app | path_to_app>"
EOF
    chmod +x "${INITRAMFS_DIR}/usr/bin/open"
fi

# Create Terminal.app bundle
TERM_APP="${INITRAMFS_DIR}/home/user/Applications/Terminal.app"
mkdir -p "${TERM_APP}"/{bin,assets}
cat << 'EOF' > "${TERM_APP}/metadata.json"
{
    "name": "LCL Terminal",
    "executable": "bin/terminal",
    "version": "1.0.0",
    "icon": "assets/icon.png"
}
EOF
cat << 'EOF' > "${TERM_APP}/bin/terminal"
#!/bin/sh
echo "===================================================="
echo "          LCL OS Terminal Subsystem App             "
echo "===================================================="
echo "Interactive PTY Shell active on seat0."
echo "===================================================="
EOF
chmod +x "${TERM_APP}/bin/terminal"

# Create SystemMonitor.app bundle
APP_DIR="${INITRAMFS_DIR}/home/user/Applications/SystemMonitor.app"
mkdir -p "${APP_DIR}"/{bin,assets}
cat << 'EOF' > "${APP_DIR}/metadata.json"
{
    "name": "System Monitor",
    "executable": "bin/sysmon",
    "version": "1.0.0",
    "icon": "assets/icon.png"
}
EOF

cat << 'EOF' > "${APP_DIR}/bin/sysmon"
#!/bin/sh
echo "===================================================="
echo "          LCL OS System Monitor v1.0.0              "
echo "===================================================="
echo "Kernel: $(uname -a)"
echo "Uptime: $(uptime 2>/dev/null || echo '0 mins')"
echo "Memory: 2048 MB RAM Allocated"
echo "===================================================="
EOF
chmod +x "${APP_DIR}/bin/sysmon"

# Copy dynamic library dependencies
echo "[LCL QEMU] Resolving dynamic library dependencies..."
FOR_BINS=("${BINARY}")
[ -f "${OPEN_BIN}" ] && FOR_BINS+=("${OPEN_BIN}")
[ -f /bin/sh ] && FOR_BINS+=("/bin/sh")
[ -f /bin/mount ] && FOR_BINS+=("/bin/mount")
[ -f /bin/mkdir ] && FOR_BINS+=("/bin/mkdir")
[ -f /bin/sleep ] && FOR_BINS+=("/bin/sleep")
[ -f /bin/ls ] && FOR_BINS+=("/bin/ls")
[ -f /usr/bin/printf ] && FOR_BINS+=("/usr/bin/printf")
[ -f /sbin/modprobe ] && FOR_BINS+=("/sbin/modprobe")

for bin in "${FOR_BINS[@]}"; do
    for lib in $(ldd "$bin" 2>/dev/null | grep -o '/[^\ ]*'); do
        if [ -f "$lib" ]; then
            cp -L "$lib" "${INITRAMFS_DIR}/usr/lib/" 2>/dev/null || true
        fi
    done
done

# Copy Host Kernel DRM & VirtIO GPU Modules (with caching)
KMOD_BASE=$(dirname "${KERNEL_PATH}")
KVER=$(basename "${KMOD_BASE}")
if [ ! -d "${KMOD_BASE}/kernel" ]; then
    KMOD_BASE="/lib/modules/${UNAME_R}"
    KVER="${UNAME_R}"
fi
TARGET_KMOD="${INITRAMFS_DIR}/usr/lib/modules/${KVER}"

if [ ! -d "${TARGET_KMOD}/kernel" ]; then
    echo "[LCL QEMU] Packaging DRM & input kernel modules for initramfs..."
    mkdir -p "${TARGET_KMOD}"
    mkdir -p "${TARGET_KMOD}/kernel/drivers/gpu"
    mkdir -p "${TARGET_KMOD}/kernel/drivers/virtio"
    if [ -d "${KMOD_BASE}/kernel/drivers/gpu/drm" ]; then
        cp -r "${KMOD_BASE}/kernel/drivers/gpu/drm" "${TARGET_KMOD}/kernel/drivers/gpu/" 2>/dev/null || true
    fi
    if [ -d "${KMOD_BASE}/kernel/drivers/virtio" ]; then
        cp -r "${KMOD_BASE}/kernel/drivers/virtio" "${TARGET_KMOD}/kernel/drivers/virtio/" 2>/dev/null || true
    fi
    if [ -d "${KMOD_BASE}/kernel/drivers/hid" ]; then
        cp -r "${KMOD_BASE}/kernel/drivers/hid" "${TARGET_KMOD}/kernel/drivers/hid/" 2>/dev/null || true
    fi
    if [ -d "${KMOD_BASE}/kernel/drivers/input" ]; then
        cp -r "${KMOD_BASE}/kernel/drivers/input" "${TARGET_KMOD}/kernel/drivers/input/" 2>/dev/null || true
    fi

    # Decompress .ko.zst modules to .ko for kernel module loader compatibility
    find "${TARGET_KMOD}" -name "*.ko.zst" -exec zstd -d --rm {} + 2>/dev/null || true

    # Regenerate modules.dep and modules.alias for initramfs module tree
    if command -v depmod >/dev/null 2>&1; then
        depmod -b "${INITRAMFS_DIR}" "${KVER}" 2>/dev/null || true
    fi
else
    echo "[LCL QEMU] Using cached kernel modules in ${TARGET_KMOD}"
fi

# Create /init startup script
cat << 'EOF' > "${INITRAMFS_DIR}/init"
#!/bin/sh
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/dri /dev/input /home/user/Desktop /home/user/Documents /home/user/Downloads /home/user/Applications
mount -t devpts devpts /dev/pts 2>/dev/null || true

# Ensure DRM & FB device nodes exist
mknod -m 666 /dev/dri/card0 c 226 0 2>/dev/null || true
mknod -m 666 /dev/dri/renderD128 c 226 128 2>/dev/null || true
mknod -m 666 /dev/fb0 c 29 0 2>/dev/null || true

# Load VirtIO DMA BUF, VirtIO GPU, DRM, and USB HID input kernel modules
modprobe virtio_dma_buf 2>/dev/null || true
modprobe virtio_gpu 2>/dev/null || true
modprobe bochs 2>/dev/null || true
modprobe drm 2>/dev/null || true
modprobe drm_kms_helper 2>/dev/null || true
modprobe usbhid 2>/dev/null || true
modprobe hid_generic 2>/dev/null || true
modprobe evdev 2>/dev/null || true

# Brief wait for udev / devtmpfs to populate /dev/dri and /dev/input
sleep 0.5

echo "===================================================="
echo "  LCL Core Linux (LCL) - QEMU Direct Kernel Boot   "
echo "===================================================="
echo "DRM devices detected:"
ls -la /dev/dri/ 2>/dev/null || echo "  (none)"
echo "Input devices detected:"
ls /dev/input/ 2>/dev/null || echo "  (none yet)"
exec /bin/lcl-core
EOF
chmod +x "${INITRAMFS_DIR}/init"

# Package initramfs image using fast compression
echo "[LCL QEMU] Packaging initramfs cpio archive..."
COMPRESS_CMD="gzip -1"
if command -v pigz >/dev/null 2>&1; then
    COMPRESS_CMD="pigz -1"
fi
(cd "${INITRAMFS_DIR}" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | ${COMPRESS_CMD} > "${INITRAMFS_IMG}")

echo "[LCL QEMU] Initramfs image built at: ${INITRAMFS_IMG}"

# 4. Configure QEMU Arguments
MEMORY="2G"
CPUS="2"

QEMU_KVM_ARGS=()
if [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_KVM_ARGS=("-enable-kvm" "-cpu" "host")
else
    QEMU_KVM_ARGS=("-cpu" "max")
fi

# VirtIO-GPU Hardware Acceleration (VirGL) configuration
GPU_ARGS=("-device" "virtio-vga-gl")
DISPLAY_ARGS=("-display" "sdl,gl=on")

echo "----------------------------------------------------"
echo "  Launching QEMU Virtual Machine:"
echo "  - Memory: ${MEMORY}"
echo "  - SMP Cores: ${CPUS}"
echo "  - Accelerator: ${QEMU_KVM_ARGS[*]}"
echo "  - GPU Accelerator: VirtIO-GPU VirGL Hardware Acceleration (${GPU_ARGS[1]})"
echo "  - Display Backend: Host OpenGL (${DISPLAY_ARGS[1]})"
echo "----------------------------------------------------"

if [ "$1" == "--run" ] || [ "$1" == "-r" ]; then
    "${QEMU_BIN}" \
        "${QEMU_KVM_ARGS[@]}" \
        -kernel "${KERNEL_PATH}" \
        -initrd "${INITRAMFS_IMG}" \
        -append "console=tty0 console=ttyS0,115200 video=1280x800-32@144 earlyprintk=ttyS0 rdinit=/init quiet loglevel=3" \
        -m "${MEMORY}" \
        -smp "${CPUS}" \
        "${GPU_ARGS[@]}" \
        "${DISPLAY_ARGS[@]}" \
        -usb \
        -device usb-ehci,id=ehci \
        -device usb-tablet,bus=ehci.0 \
        -device virtio-keyboard-pci \
        -serial stdio
else
    echo "[LCL QEMU] Boot environment ready!"
    echo "[LCL QEMU] Run '${0} --run' to launch QEMU in live VM."
fi

