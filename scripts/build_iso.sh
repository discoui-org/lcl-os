#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
ISO_ROOT="${BUILD_DIR}/iso_root"
OUTPUT_ISO="${BUILD_DIR}/lcl-os.iso"

echo "===================================================="
echo "  LCL OS - Limine Bootable ISO / LiveUSB Builder    "
echo "===================================================="

# 1. Ensure kernel and initramfs artifacts exist
KERNEL_SRC="${BUILD_DIR}/qemu-cache/vmlinuz"
INITRAMFS_SRC="${BUILD_DIR}/initramfs.cpio.gz"

if [ ! -f "$KERNEL_SRC" ] || [ ! -f "$INITRAMFS_SRC" ]; then
    echo "[ISO Build] Packaging kernel and initramfs via Docker..."
    python3 "${SCRIPT_DIR}/run_qemu.py" --package-only
fi

if [ ! -f "$KERNEL_SRC" ] || [ ! -f "$INITRAMFS_SRC" ]; then
    echo "ERROR: Kernel ($KERNEL_SRC) or initramfs ($INITRAMFS_SRC) not found!"
    exit 1
fi

# 2. Locate Limine binaries
LIMINE_DATADIR=""
if command -v limine &>/dev/null; then
    LIMINE_DATADIR="$(limine --print-datadir 2>/dev/null || true)"
fi

if [ -z "$LIMINE_DATADIR" ] || [ ! -d "$LIMINE_DATADIR" ]; then
    for candidate in /usr/share/limine /usr/local/share/limine; do
        if [ -d "$candidate" ]; then
            LIMINE_DATADIR="$candidate"
            break
        fi
    done
fi

if [ -z "$LIMINE_DATADIR" ] || [ ! -d "$LIMINE_DATADIR" ]; then
    echo "ERROR: Limine datadir not found. Please install limine."
    exit 1
fi

echo "[ISO Build] Using Limine datadir: ${LIMINE_DATADIR}"

# Required Limine boot files
BOOTX64="${LIMINE_DATADIR}/BOOTX64.EFI"
BIOS_CD="${LIMINE_DATADIR}/limine-bios-cd.bin"
BIOS_SYS="${LIMINE_DATADIR}/limine-bios.sys"
UEFI_CD="${LIMINE_DATADIR}/limine-uefi-cd.bin"

for req in "$BOOTX64" "$BIOS_CD" "$BIOS_SYS" "$UEFI_CD"; do
    if [ ! -f "$req" ]; then
        echo "ERROR: Missing required Limine file: ${req}"
        exit 1
    fi
done

# 3. Stage ISO Root Directory
echo "[ISO Build] Staging filesystem at ${ISO_ROOT}..."
rm -rf "${ISO_ROOT}"
mkdir -p "${ISO_ROOT}/EFI/BOOT"
mkdir -p "${ISO_ROOT}/boot"

# Copy Limine EFI and BIOS binaries
cp "$BOOTX64" "${ISO_ROOT}/EFI/BOOT/BOOTX64.EFI"
cp "$BIOS_CD" "${ISO_ROOT}/boot/limine-bios-cd.bin"
cp "$BIOS_SYS" "${ISO_ROOT}/boot/limine-bios.sys"
cp "$BIOS_SYS" "${ISO_ROOT}/boot/limine.sys"
cp "$BIOS_SYS" "${ISO_ROOT}/limine-bios.sys"
cp "$BIOS_SYS" "${ISO_ROOT}/limine.sys"
cp "$UEFI_CD" "${ISO_ROOT}/boot/limine-uefi-cd.bin"

# Copy limine.conf
LIMINE_CONF_SRC="${PROJECT_ROOT}/iso_root/boot/limine.conf"
if [ ! -f "$LIMINE_CONF_SRC" ]; then
    echo "WARNING: ${LIMINE_CONF_SRC} not found. Creating default..."
    cat << 'EOF' > "${ISO_ROOT}/boot/limine.conf"
timeout: 3

/LCL OS
    protocol: linux
    kernel_path: boot():/boot/vmlinuz
    initrd_path: boot():/boot/initramfs.cpio.gz
    cmdline: console=tty0 lcl.scale=1
    resolution: preferred
EOF
else
    cp "$LIMINE_CONF_SRC" "${ISO_ROOT}/boot/limine.conf"
fi
cp "${ISO_ROOT}/boot/limine.conf" "${ISO_ROOT}/limine.conf"

# Copy kernel & initramfs
echo "[ISO Build] Copying kernel and initramfs..."
cp "$KERNEL_SRC" "${ISO_ROOT}/boot/vmlinuz"
cp "$INITRAMFS_SRC" "${ISO_ROOT}/boot/initramfs.cpio.gz"

# 4. Create Hybrid ISO using xorriso / mkisofs
echo "[ISO Build] Generating hybrid ISO image..."
if ! command -v xorriso &>/dev/null; then
    echo "ERROR: xorriso command not found. Please install xorriso."
    exit 1
fi

xorriso -as mkisofs -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "${ISO_ROOT}" -o "${OUTPUT_ISO}"

# 5. Install Limine BIOS stage to ISO MBR/GPT header
echo "[ISO Build] Installing Limine BIOS bootloader..."
if command -v limine &>/dev/null; then
    limine bios-install "${OUTPUT_ISO}"
else
    echo "WARNING: limine binary not found on PATH. Skipping bios-install."
fi

ISO_SIZE="$(du -h "${OUTPUT_ISO}" | cut -f1)"
echo "===================================================="
echo "  ISO Build Complete!"
echo "  Artifact: ${OUTPUT_ISO} (${ISO_SIZE})"
echo "===================================================="
