#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
ISO_ROOT="${BUILD_DIR}/iso_root"
ARCH="${ARCH:-$(uname -m)}"
case "$ARCH" in
    arm64|aarch64|arm)
        TARGET_ARCH="aarch64"
        ;;
    *)
        TARGET_ARCH="x86_64"
        ;;
esac

OUTPUT_ISO="${BUILD_DIR}/lcl-os-${TARGET_ARCH}.iso"
GENERIC_ISO="${BUILD_DIR}/lcl-os.iso"

echo "===================================================="
echo "  LCL OS - Limine Bootable ISO (${TARGET_ARCH}) Builder"
echo "===================================================="

# 1. Compile binaries and package kernel and initramfs
KERNEL_SRC="${BUILD_DIR}/qemu-cache/vmlinuz"
INITRAMFS_SRC="${BUILD_DIR}/initramfs.cpio.gz"

echo "[ISO Build] Compiling binaries & packaging kernel/initramfs for ${TARGET_ARCH} via Docker..."
python3 "${SCRIPT_DIR}/run_qemu.py" --package-only --arch "${TARGET_ARCH}"

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

# 3. Stage ISO Root Directory
echo "[ISO Build] Staging filesystem at ${ISO_ROOT}..."
rm -rf "${ISO_ROOT}"
mkdir -p "${ISO_ROOT}/EFI/BOOT"
mkdir -p "${ISO_ROOT}/boot"

if [ "$TARGET_ARCH" = "aarch64" ]; then
    BOOTAA64="${LIMINE_DATADIR}/BOOTAA64.EFI"
    UEFI_CD="${LIMINE_DATADIR}/limine-uefi-cd.bin"
    if [ -f "$BOOTAA64" ]; then
        cp "$BOOTAA64" "${ISO_ROOT}/EFI/BOOT/BOOTAA64.EFI"
    fi
    if [ -f "$UEFI_CD" ]; then
        cp "$UEFI_CD" "${ISO_ROOT}/boot/limine-uefi-cd.bin"
    fi
else
    BOOTX64="${LIMINE_DATADIR}/BOOTX64.EFI"
    BIOS_CD="${LIMINE_DATADIR}/limine-bios-cd.bin"
    BIOS_SYS="${LIMINE_DATADIR}/limine-bios.sys"
    UEFI_CD="${LIMINE_DATADIR}/limine-uefi-cd.bin"

    for req in "$BOOTX64" "$BIOS_CD" "$BIOS_SYS" "$UEFI_CD"; do
        if [ -f "$req" ]; then
            case "$req" in
                *BOOTX64.EFI) cp "$req" "${ISO_ROOT}/EFI/BOOT/BOOTX64.EFI" ;;
                *limine-bios-cd.bin) cp "$req" "${ISO_ROOT}/boot/limine-bios-cd.bin" ;;
                *limine-bios.sys)
                    cp "$req" "${ISO_ROOT}/boot/limine-bios.sys"
                    cp "$req" "${ISO_ROOT}/boot/limine.sys"
                    cp "$req" "${ISO_ROOT}/limine-bios.sys"
                    cp "$req" "${ISO_ROOT}/limine.sys"
                    ;;
                *limine-uefi-cd.bin) cp "$req" "${ISO_ROOT}/boot/limine-uefi-cd.bin" ;;
            esac
        fi
    done
fi

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

# 4. Create ISO using xorriso
echo "[ISO Build] Generating ISO image..."
if ! command -v xorriso &>/dev/null; then
    echo "ERROR: xorriso command not found. Please install xorriso."
    exit 1
fi

if [ "$TARGET_ARCH" = "x86_64" ] && [ -f "${ISO_ROOT}/boot/limine-bios-cd.bin" ]; then
    xorriso -as mkisofs -b boot/limine-bios-cd.bin \
            -no-emul-boot -boot-load-size 4 -boot-info-table \
            --efi-boot boot/limine-uefi-cd.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            "${ISO_ROOT}" -o "${OUTPUT_ISO}"
    if command -v limine &>/dev/null; then
        limine bios-install "${OUTPUT_ISO}" || true
    fi
else
    # UEFI-only CD / aarch64 image
    xorriso -as mkisofs -r -J \
            --efi-boot boot/limine-uefi-cd.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            "${ISO_ROOT}" -o "${OUTPUT_ISO}" 2>/dev/null || \
    xorriso -as mkisofs -r -J "${ISO_ROOT}" -o "${OUTPUT_ISO}"
fi

cp -f "${OUTPUT_ISO}" "${GENERIC_ISO}"

ISO_SIZE="$(du -h "${OUTPUT_ISO}" | cut -f1)"
echo "===================================================="
echo "  ISO Build Complete!"
echo "  Artifact: ${OUTPUT_ISO} (${ISO_SIZE})"
echo "===================================================="
