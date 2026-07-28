.PHONY: build compile qemu qemu-prep iso qemu-iso clean fonts help flash flash-usb

# Usage:
#   make qemu
#   make qemu NATIVE=1
#   make iso
#   make qemu-iso
#   make qemu-iso UEFI=1
#   make flash (or make flash-usb)

GPU ?= 0
NATIVE ?= 0
UEFI ?= 0
USB ?=
USB_DEV ?= /dev/disk/by-id/usb-SanDisk_Cruzer_Blade_04019222101620123055-0:0
QEMU_NATIVE_FLAG := $(if $(filter 1 yes true on,$(NATIVE)),--native,)
QEMU_GPU_FLAG := $(if $(filter 1 yes true on,$(GPU)),--gpu,)
QEMU_UEFI_FLAG := $(if $(filter 1 yes true on,$(UEFI)),--uefi,)
QEMU_USB_FLAG := $(if $(USB),--usb $(USB),)

build compile:
	python3 scripts/run_qemu.py --build-only

qemu-prep:
	python3 scripts/run_qemu.py

qemu:
	python3 scripts/run_qemu.py --run $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG) $(QEMU_USB_FLAG)

iso:
	./scripts/build_iso.sh

qemu-iso: iso
	python3 scripts/run_qemu.py --iso --run $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG) $(QEMU_UEFI_FLAG) $(QEMU_USB_FLAG)

flash flash-usb: iso
	@if [ ! -b "$(USB_DEV)" ] && [ ! -e "$(USB_DEV)" ]; then \
		echo "ERROR: USB device not found: $(USB_DEV)"; \
		echo "Please ensure the SanDisk USB drive is plugged in or run 'make flash USB_DEV=/dev/sdX'."; \
		exit 1; \
	fi
	@echo "[Flash] Writing ISO image to $(USB_DEV)..."
	@if command -v pv >/dev/null 2>&1; then \
		pv build/lcl-os.iso | sudo dd of=$(USB_DEV) bs=4M conv=fsync status=none; \
	else \
		sudo dd if=build/lcl-os.iso of=$(USB_DEV) bs=4M status=progress conv=fsync; \
	fi
	sync
	@echo "[Flash] Successfully written! You may safely unplug the USB drive."

fonts:
	./scripts/fetch_fonts.sh

clean:
	python3 scripts/run_qemu.py --clean

help:
	@echo "make build           - Build lcl-core via Docker (always)"
	@echo "make qemu-prep       - Docker package kernel+initramfs"
	@echo "make qemu            - Docker package + launch host QEMU"
	@echo "make qemu NATIVE=1   - Host res + scale + QEMU fullscreen"
	@echo "make iso             - Generate bootable Limine ISO image (build/lcl-os.iso)"
	@echo "make qemu-iso        - Build ISO & launch QEMU in BIOS mode"
	@echo "make qemu-iso UEFI=1 - Build ISO & launch QEMU in UEFI mode (OVMF)"
	@echo "make flash           - Build ISO & flash directly to SanDisk USB drive"
	@echo "make fonts           - Fetch font assets"
	@echo "make clean           - Remove build/"
	@echo ""
	@echo "Requires: Docker + qemu-system-x86_64 (guest never uses host kernel)"

