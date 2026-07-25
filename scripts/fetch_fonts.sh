#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ASSETS_FONTS_DIR="${ROOT_DIR}/assets/fonts"

echo "===================================================="
echo "    LCL OS - Font Asset Manager & Downloader        "
echo "===================================================="

mkdir -p "${ASSETS_FONTS_DIR}"/{inter,jetbrains-mono,liberation-serif,liberation-sans}
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

# Helper: download file
fetch_url() {
    local url="$1"
    local dest="$2"
    echo "[LCL Fonts] Downloading: ${url}..."
    curl -sSL --connect-timeout 10 --retry 3 "${url}" -o "${dest}"
}

# 1. Fetch Inter (UI Font)
INTER_DIR="${ASSETS_FONTS_DIR}/inter"
if [ -z "$(ls -A "${INTER_DIR}" 2>/dev/null)" ]; then
    echo "[LCL Fonts] Fetching Inter (UI Sans-Serif Font)..."
    INTER_ZIP="${TMP_DIR}/Inter.zip"
    fetch_url "https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip" "${INTER_ZIP}"
    if [ -f "${INTER_ZIP}" ]; then
        unzip -q -o "${INTER_ZIP}" -d "${TMP_DIR}/inter_extracted" 2>/dev/null || true
        find "${TMP_DIR}/inter_extracted" -type f \( -name "*.ttf" -o -name "*.otf" \) -exec cp {} "${INTER_DIR}/" \;
        echo "[LCL Fonts] Inter font installed to ${INTER_DIR} ($(ls -1 "${INTER_DIR}" 2>/dev/null | wc -l) files)."
    fi
else
    echo "[LCL Fonts] Inter font already present in ${INTER_DIR}."
fi

# 2. Fetch JetBrains Mono (Terminal Monospace Font)
JBM_DIR="${ASSETS_FONTS_DIR}/jetbrains-mono"
if [ -z "$(ls -A "${JBM_DIR}" 2>/dev/null)" ]; then
    echo "[LCL Fonts] Fetching JetBrains Mono (Terminal Monospace Font)..."
    JBM_ZIP="${TMP_DIR}/JetBrainsMono.zip"
    fetch_url "https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip" "${JBM_ZIP}"
    if [ -f "${JBM_ZIP}" ]; then
        unzip -q -o "${JBM_ZIP}" -d "${TMP_DIR}/jbm_extracted" 2>/dev/null || true
        find "${TMP_DIR}/jbm_extracted" -type f -name "*.ttf" -exec cp {} "${JBM_DIR}/" \;
        echo "[LCL Fonts] JetBrains Mono font installed to ${JBM_DIR} ($(ls -1 "${JBM_DIR}" 2>/dev/null | wc -l) files)."
    fi
else
    echo "[LCL Fonts] JetBrains Mono font already present in ${JBM_DIR}."
fi

# 3. Fetch Liberation Fonts (Serif & Sans)
LIB_SERIF_DIR="${ASSETS_FONTS_DIR}/liberation-serif"
LIB_SANS_DIR="${ASSETS_FONTS_DIR}/liberation-sans"
if [ -z "$(ls -A "${LIB_SERIF_DIR}" 2>/dev/null)" ] || [ -z "$(ls -A "${LIB_SANS_DIR}" 2>/dev/null)" ]; then
    echo "[LCL Fonts] Fetching Liberation Fonts (Standard Serif & Sans)..."
    LIB_TAR="${TMP_DIR}/liberation.tar.gz"
    fetch_url "https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz" "${LIB_TAR}"
    if [ -f "${LIB_TAR}" ]; then
        tar -xzf "${LIB_TAR}" -C "${TMP_DIR}" 2>/dev/null || true
        find "${TMP_DIR}" -type f -name "LiberationSerif-*.ttf" -exec cp {} "${LIB_SERIF_DIR}/" \;
        find "${TMP_DIR}" -type f -name "LiberationSans-*.ttf" -exec cp {} "${LIB_SANS_DIR}/" \;
        echo "[LCL Fonts] Liberation Serif font installed to ${LIB_SERIF_DIR} ($(ls -1 "${LIB_SERIF_DIR}" 2>/dev/null | wc -l) files)."
        echo "[LCL Fonts] Liberation Sans font installed to ${LIB_SANS_DIR} ($(ls -1 "${LIB_SANS_DIR}" 2>/dev/null | wc -l) files)."
    fi
else
    echo "[LCL Fonts] Liberation fonts already present."
fi

echo "===================================================="
echo "    LCL Fonts Installation & Setup Complete!        "
echo "===================================================="
