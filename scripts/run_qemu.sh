#!/usr/bin/env bash
# Thin wrapper — logic lives in run_qemu.py (Linux / macOS / Windows).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/run_qemu.py" "$@"
