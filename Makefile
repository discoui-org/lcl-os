.PHONY: build compile qemu qemu-prep clean fonts help

# Usage:
#   make qemu
#   make qemu NATIVE=1

NATIVE ?= 0
QEMU_NATIVE_FLAG := $(if $(filter 1 yes true on,$(NATIVE)),--native,)

build compile:
	python3 scripts/run_qemu.py --build-only

qemu-prep:
	python3 scripts/run_qemu.py

qemu:
	python3 scripts/run_qemu.py --run $(QEMU_NATIVE_FLAG)

fonts:
	./scripts/fetch_fonts.sh

clean:
	rm -rf build

help:
	@echo "make build           - Build lcl-core via Docker (always)"
	@echo "make qemu-prep       - Docker package kernel+initramfs"
	@echo "make qemu            - Docker package + launch host QEMU"
	@echo "make qemu NATIVE=1   - Host res + scale + QEMU fullscreen"
	@echo "make fonts           - Fetch font assets"
	@echo "make clean           - Remove build/"
	@echo ""
	@echo "Requires: Docker + qemu-system-x86_64 (guest never uses host kernel)"
