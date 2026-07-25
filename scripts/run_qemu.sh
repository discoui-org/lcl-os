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
rm -rf "${INITRAMFS_DIR}"
mkdir -p "${INITRAMFS_DIR}"/{proc,sys,dev,tmp,etc,usr/bin,usr/lib,usr/share}
ln -s usr/bin "${INITRAMFS_DIR}/bin"
ln -s usr/bin "${INITRAMFS_DIR}/sbin"
ln -s usr/lib "${INITRAMFS_DIR}/lib"
ln -s usr/lib "${INITRAMFS_DIR}/lib64"

# Copy lcl-core, sh, mount, and mkdir binaries
cp "${BINARY}" "${INITRAMFS_DIR}/usr/bin/lcl-core"
[ -f /bin/sh ] && cp -L /bin/sh "${INITRAMFS_DIR}/usr/bin/sh"
[ -f /bin/mount ] && cp -L /bin/mount "${INITRAMFS_DIR}/usr/bin/mount"
[ -f /bin/mkdir ] && cp -L /bin/mkdir "${INITRAMFS_DIR}/usr/bin/mkdir"
[ -d /usr/share/libinput ] && cp -r /usr/share/libinput "${INITRAMFS_DIR}/usr/share/" 2>/dev/null || true

# Copy dynamic library dependencies
echo "[LCL QEMU] Resolving dynamic library dependencies..."
FOR_BINS=("${BINARY}")
[ -f /bin/sh ] && FOR_BINS+=("/bin/sh")
[ -f /bin/mount ] && FOR_BINS+=("/bin/mount")
[ -f /bin/mkdir ] && FOR_BINS+=("/bin/mkdir")

for bin in "${FOR_BINS[@]}"; do
    for lib in $(ldd "$bin" 2>/dev/null | grep -o '/[^\ ]*'); do
        if [ -f "$lib" ]; then
            cp -L "$lib" "${INITRAMFS_DIR}/usr/lib/" 2>/dev/null || true
        fi
    done
done

# Create /init startup script
cat << 'EOF' > "${INITRAMFS_DIR}/init"
#!/bin/sh
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/dri /dev/input
mount -t devpts devpts /dev/pts 2>/dev/null || true

echo "===================================================="
echo "  LCL Core Linux (LCL) - QEMU Direct Kernel Boot   "
echo "===================================================="
exec /bin/lcl-core
EOF
chmod +x "${INITRAMFS_DIR}/init"

# Package initramfs image
echo "[LCL QEMU] Packaging initramfs cpio archive..."
(cd "${INITRAMFS_DIR}" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${INITRAMFS_IMG}")

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

echo "----------------------------------------------------"
echo "  Launching QEMU Virtual Machine:"
echo "  - Memory: ${MEMORY}"
echo "  - SMP Cores: ${CPUS}"
echo "  - Accelerator: ${QEMU_KVM_ARGS[*]}"
echo "  - Display: Standard VGA / VirtIO Framebuffer"
echo "----------------------------------------------------"

if [ "$1" == "--run" ] || [ "$1" == "-r" ]; then
    "${QEMU_BIN}" \
        "${QEMU_KVM_ARGS[@]}" \
        -kernel "${KERNEL_PATH}" \
        -initrd "${INITRAMFS_IMG}" \
        -append "console=tty0 console=ttyS0,115200 vga=792 video=1280x720-32 earlyprintk=ttyS0 rdinit=/init quiet loglevel=3" \
        -m "${MEMORY}" \
        -smp "${CPUS}" \
        -vga std \
        -device virtio-gpu-pci \
        -serial stdio
else
    echo "[LCL QEMU] Boot environment ready!"
    echo "[LCL QEMU] Run '${0} --run' to launch QEMU in live VM."
fi
