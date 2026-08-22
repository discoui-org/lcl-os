#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# LCL Core Linux — Android 16 Frozen AIDL NDK C++ Bindings Generator
# Pinned to AOSP Release Tag: android-16.0.0_r1
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${REPO_ROOT}/src/platform/android/generated"

ANDROID_TAG="android-16.0.0_r1"
COMPOSER3_VERSION="4"
GRAPHICS_COMMON_VERSION="6"
HW_COMMON_VERSION="2"
DRM_COMMON_VERSION="1"

AIDL_BIN="${ANDROID_HOME:-$HOME/Android/Sdk}/build-tools/36.1.0/aidl"
if [ ! -x "${AIDL_BIN}" ]; then
    AIDL_BIN="$(which aidl 2>/dev/null || true)"
fi

if [ -z "${AIDL_BIN}" ] || [ ! -x "${AIDL_BIN}" ]; then
    echo "ERROR: aidl compiler not found. Set ANDROID_HOME or install Android SDK Build-Tools 36+." >&2
    exit 1
fi

TEMP_DIR="$(mktemp -d /tmp/lcl_aidl_gen_XXXXXX)"
trap 'rm -rf "${TEMP_DIR}"' EXIT

echo "=== Fetching Pinned Frozen AIDL Snapshots (${ANDROID_TAG}) ==="
mkdir -p "${TEMP_DIR}/composer" "${TEMP_DIR}/graphics_common" "${TEMP_DIR}/common" "${TEMP_DIR}/drm_common"
mkdir -p "${TEMP_DIR}/out_inc" "${TEMP_DIR}/out_src"

# 1. Composer3-V4
echo " -> android.hardware.graphics.composer3-V${COMPOSER3_VERSION}"
curl -sL "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/tags/${ANDROID_TAG}/graphics/composer/aidl/aidl_api/android.hardware.graphics.composer3/${COMPOSER3_VERSION}.tar.gz" \
    | tar -xz -C "${TEMP_DIR}/composer"

# 2. Graphics Common-V6
echo " -> android.hardware.graphics.common-V${GRAPHICS_COMMON_VERSION}"
curl -sL "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/tags/${ANDROID_TAG}/graphics/common/aidl/aidl_api/android.hardware.graphics.common/${GRAPHICS_COMMON_VERSION}.tar.gz" \
    | tar -xz -C "${TEMP_DIR}/graphics_common"

# 3. Hardware Common-V2
echo " -> android.hardware.common-V${HW_COMMON_VERSION}"
curl -sL "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/tags/${ANDROID_TAG}/common/aidl/aidl_api/android.hardware.common/${HW_COMMON_VERSION}.tar.gz" \
    | tar -xz -C "${TEMP_DIR}/common"

# 4. DRM Common-V1
echo " -> android.hardware.drm.common-V${DRM_COMMON_VERSION}"
curl -sL "https://android.googlesource.com/platform/hardware/interfaces/+archive/refs/tags/${ANDROID_TAG}/drm/common/aidl/aidl_api/android.hardware.drm.common/${DRM_COMMON_VERSION}.tar.gz" \
    | tar -xz -C "${TEMP_DIR}/drm_common"

# 5. Fetch NDK Binder C++ helper headers from frameworks/native
echo " -> NDK Binder C++ Utilities (include_cpp)"
mkdir -p "${TEMP_DIR}/binder_cpp"
curl -sL "https://android.googlesource.com/platform/frameworks/native/+archive/refs/tags/${ANDROID_TAG}/libs/binder/ndk/include_cpp.tar.gz" \
    | tar -xz -C "${TEMP_DIR}/binder_cpp"

echo "=== Compiling Frozen AIDL Interfaces to NDK C++ ==="
INCLUDES="-I ${TEMP_DIR}/composer -I ${TEMP_DIR}/graphics_common -I ${TEMP_DIR}/common -I ${TEMP_DIR}/drm_common"

for pkg_dir in "${TEMP_DIR}/common" "${TEMP_DIR}/graphics_common" "${TEMP_DIR}/drm_common" "${TEMP_DIR}/composer"; do
    for aidl_file in $(find "${pkg_dir}/android" -name "*.aidl"); do
        "${AIDL_BIN}" --lang=ndk --structured --stability=vintf \
          ${INCLUDES} \
          -h "${TEMP_DIR}/out_inc" -o "${TEMP_DIR}/out_src" "${aidl_file}"
    done
done

echo "=== Installing Generated Bindings into Repository ==="
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/include" "${OUTPUT_DIR}/src"

cp -r "${TEMP_DIR}/out_inc/"* "${OUTPUT_DIR}/include/"
cp -r "${TEMP_DIR}/out_src/"* "${OUTPUT_DIR}/src/"
cp -r "${TEMP_DIR}/binder_cpp/android" "${OUTPUT_DIR}/include/"

cat << EOF > "${OUTPUT_DIR}/PROVENANCE.txt"
LCL Core Linux — Android Generated AIDL NDK Bindings
Generated On: $(date -u +"%Y-%m-%dT%H:%M:%SZ")
AOSP Release Tag: ${ANDROID_TAG}

Pinned Interfaces:
- android.hardware.graphics.composer3-V${COMPOSER3_VERSION}
- android.hardware.graphics.common-V${GRAPHICS_COMMON_VERSION}
- android.hardware.common-V${HW_COMMON_VERSION}
- android.hardware.drm.common-V${DRM_COMMON_VERSION}
- frameworks/native NDK Binder include_cpp (${ANDROID_TAG})
EOF

echo "✓ AIDL Generation Completed Successfully."
echo "  Headers: $(find "${OUTPUT_DIR}/include" -type f | wc -l) files"
echo "  Sources: $(find "${OUTPUT_DIR}/src" -type f | wc -l) files"
