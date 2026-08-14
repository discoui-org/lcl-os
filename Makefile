.PHONY: build compile qemu qemu-prep iso qemu-iso clean fonts help flash flash-usb

# Usage:
#   make qemu
#   make qemu NATIVE=1
#   make iso
#   make qemu-iso
#   make qemu-iso UEFI=1
#   make flash (or make flash-usb)

HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),arm64)
  DEFAULT_ARCH := aarch64
else ifeq ($(HOST_ARCH),aarch64)
  DEFAULT_ARCH := aarch64
else
  DEFAULT_ARCH := x86_64
endif

ARCH ?= $(DEFAULT_ARCH)
GPU ?= 0
NATIVE ?= 0
UEFI ?= 0
RETINA ?= 0
SCALE ?=
WIDTH ?=
HEIGHT ?=
USB ?=
USB_DEV ?= /dev/disk/by-id/usb-SanDisk_Cruzer_Blade_04019222101620123055-0:0
QEMU_ARCH_FLAG := --arch $(ARCH)
QEMU_NATIVE_FLAG := $(if $(filter 1 yes true on,$(NATIVE)),--native,)
QEMU_GPU_FLAG := $(if $(filter 1 yes true on,$(GPU)),--gpu,)
QEMU_UEFI_FLAG := $(if $(filter 1 yes true on,$(UEFI)),--uefi,)
QEMU_RETINA_FLAG := $(if $(filter 1 yes true on,$(RETINA)),--retina,)
QEMU_SCALE_FLAG := $(if $(SCALE),--scale $(SCALE),)
QEMU_WIDTH_FLAG := $(if $(WIDTH),--width $(WIDTH),)
QEMU_HEIGHT_FLAG := $(if $(HEIGHT),--height $(HEIGHT),)
QEMU_USB_FLAG := $(if $(USB),--usb $(USB),)

build compile:
	python3 scripts/run_qemu.py --build-only $(QEMU_ARCH_FLAG)

qemu-prep:
	python3 scripts/run_qemu.py $(QEMU_ARCH_FLAG)

qemu:
	python3 scripts/run_qemu.py --run $(QEMU_ARCH_FLAG) $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG) $(QEMU_RETINA_FLAG) $(QEMU_SCALE_FLAG) $(QEMU_WIDTH_FLAG) $(QEMU_HEIGHT_FLAG) $(QEMU_USB_FLAG)

iso:
	ARCH=$(ARCH) ./scripts/build_iso.sh

qemu-iso: iso
	python3 scripts/run_qemu.py --iso --run $(QEMU_ARCH_FLAG) $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG) $(QEMU_UEFI_FLAG) $(QEMU_RETINA_FLAG) $(QEMU_SCALE_FLAG) $(QEMU_WIDTH_FLAG) $(QEMU_HEIGHT_FLAG) $(QEMU_USB_FLAG)

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
	python3 scripts/run_qemu.py --clean $(QEMU_ARCH_FLAG)

help:
	@echo "make build           - Build lcl-core via Docker (always)"
	@echo "make qemu-prep       - Docker package kernel+initramfs"
	@echo "make qemu            - Docker package + launch host QEMU"
	@echo "make qemu ARCH=arm64 - Launch QEMU for ARM64/AArch64 (native on Apple Silicon/ARM)"
	@echo "make qemu NATIVE=1   - Host res + scale + QEMU fullscreen"
	@echo "make qemu RETINA=1   - 13\" MacBook Air Retina (2560x1600 @ 2.0x scale)"
	@echo "make qemu SCALE=1.5  - Custom UI scale (e.g. 1.25, 1.5, 2.0)"
	@echo "make qemu WIDTH=... HEIGHT=... - Custom screen resolution"
	@echo "make iso             - Generate bootable Limine ISO image (build/lcl-os.iso)"
	@echo "make iso ARCH=arm64  - Build Limine bootable ISO for ARM64"
	@echo "make qemu-iso        - Build ISO & launch QEMU in BIOS mode"
	@echo "make qemu-iso UEFI=1 - Build ISO & launch QEMU in UEFI mode (OVMF)"
	@echo "make flash           - Build ISO & flash directly to SanDisk USB drive"
	@echo "make fonts           - Fetch font assets"
	@echo "make clean           - Remove build/"
	@echo ""
	@echo "Requires: Docker + QEMU (guest never uses host kernel)"

