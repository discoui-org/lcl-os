.PHONY: build compile qemu qemu-prep clean fonts help

# Usage:
#   make qemu
#   make qemu NATIVE=1

GPU ?= 0
NATIVE ?= 0
QEMU_NATIVE_FLAG := $(if $(filter 1 yes true on,$(NATIVE)),--native,)
QEMU_GPU_FLAG := $(if $(filter 1 yes true on,$(GPU)),--gpu,)

build compile:
	python3 scripts/run_qemu.py --build-only

qemu-prep:
	python3 scripts/run_qemu.py

qemu:
	python3 scripts/run_qemu.py --run $(QEMU_NATIVE_FLAG) $(QEMU_GPU_FLAG)

fonts:
	./scripts/fetch_fonts.sh

clean:
	python3 scripts/run_qemu.py --clean

help:
	@echo "make build           - Build lcl-core via Docker (always)"
	@echo "make qemu-prep       - Docker package kernel+initramfs"
	@echo "make qemu            - Docker package + launch host QEMU"
	@echo "make qemu NATIVE=1   - Host res + scale + QEMU fullscreen"
	@echo "make fonts           - Fetch font assets"
	@echo "make clean           - Remove build/"
	@echo ""
	@echo "Requires: Docker + qemu-system-x86_64 (guest never uses host kernel)"
