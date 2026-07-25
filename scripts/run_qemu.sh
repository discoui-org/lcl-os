#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build"
BINARY="${BUILD_DIR}/lcl-core"

echo "===================================================="
echo "  LCL Core Linux - QEMU Execution & Test Environment"
echo "===================================================="

# Check and build project if binary missing
if [ ! -f "${BINARY}" ]; then
    echo "[LCL QEMU] Binary ${BINARY} not found. Triggering CMake build..."
    cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -DCMAKE_BUILD_TYPE=Debug
    cmake --build "${BUILD_DIR}"
fi

QEMU_BIN=$(which qemu-system-x86_64 2>/dev/null || true)
if [ -z "${QEMU_BIN}" ]; then
    echo "[LCL QEMU ERROR] qemu-system-x86_64 binary not found in PATH."
    echo "[LCL QEMU HINT] Install qemu using your package manager (e.g., sudo apt install qemu-system-x86)."
    exit 1
fi

echo "[LCL QEMU] QEMU binary detected: ${QEMU_BIN}"
echo "[LCL QEMU] Target LCL Core binary: ${BINARY}"

# Setup parameters
MEMORY="2G"
CPUS="2"

echo "----------------------------------------------------"
echo "  QEMU Test Environment Configuration:"
echo "  - Memory: ${MEMORY}"
echo "  - SMP Cores: ${CPUS}"
echo "  - Graphics Controller: virtio-gpu-pci (Direct DRM/KMS)"
echo "----------------------------------------------------"
echo "[LCL QEMU] Verification complete. QEMU test runner script is ready."
