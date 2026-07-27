.PHONY: build compile qemu qemu-prep iso qemu-iso clean fonts help

# Usage:
#   make qemu
#   make qemu NATIVE=1
#   make iso
#   make qemu-iso
#   make qemu-iso UEFI=1

GPU ?= 0
NATIVE ?= 0
UEFI ?= 0
QEMU_NATIVE_FLAG := $(if $(filter 1 yes true on,$(NATIVE)),--native,)
QEMU_GPU_FLAG := $(if $(filter 1 yes true on,$(GPU)),--gpu,)
QEMU_UEFI_FLAG := $(if $(filter 1 yes true on,$(UEFI)),--uefi,)

build compile:
	python3 scripts/run_qemu.py --build-only

qemu-prep:
	python3 scripts/run_qemu.py

qemu:
	python3 scripts/run_qemu.py --run $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG)

iso:
	./scripts/build_iso.sh

qemu-iso: iso
	python3 scripts/run_qemu.py --iso --run $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG) $(QEMU_UEFI_FLAG)

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
	@echo "make fonts           - Fetch font assets"
	@echo "make clean           - Remove build/"
	@echo ""
	@echo "Requires: Docker + qemu-system-x86_64 (guest never uses host kernel)"

