.PHONY: build compile qemu qemu-prep clean fonts help

build compile:
	python3 scripts/run_qemu.py --build-only

qemu-prep:
	python3 scripts/run_qemu.py

qemu:
	python3 scripts/run_qemu.py --run

fonts:
	./scripts/fetch_fonts.sh

clean:
	rm -rf build

help:
	@echo "make build      - Build lcl-core (native on Linux, Docker elsewhere)"
	@echo "make qemu-prep  - Build + package initramfs/kernel artifacts"
	@echo "make qemu       - Build, package, and launch QEMU"
	@echo "make fonts      - Fetch font assets"
	@echo "make clean      - Remove build/"
