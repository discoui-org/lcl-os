#!/usr/bin/env python3
"""
LCL OS - Font Asset Manager & Downloader (Pure Python)
Downloads and extracts Inter, JetBrains Mono, and Liberation fonts.
"""

from __future__ import annotations

import io
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parents[1]
ASSETS_FONTS_DIR = ROOT_DIR / "assets" / "fonts"


def log(msg: str) -> None:
    print(f"[LCL Fonts] {msg}")


def download_bytes(url: str) -> bytes:
    log(f"Downloading {url}...")
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "LCL-OS-Asset-Fetcher/1.0"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read()


def fetch_inter() -> None:
    inter_dir = ASSETS_FONTS_DIR / "inter"
    inter_dir.mkdir(parents=True, exist_ok=True)
    existing = list(inter_dir.glob("*.ttf")) + list(inter_dir.glob("*.otf"))
    if existing:
        log(f"Inter font already present in {inter_dir} ({len(existing)} files).")
        return

    log("Fetching Inter (UI Sans-Serif Font)...")
    url = "https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip"
    try:
        data = download_bytes(url)
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            count = 0
            for name in zf.namelist():
                if name.endswith((".ttf", ".otf")):
                    filename = Path(name).name
                    if filename:
                        content = zf.read(name)
                        (inter_dir / filename).write_bytes(content)
                        count += 1
        log(f"Inter font installed to {inter_dir} ({count} files).")
    except Exception as exc:
        log(f"WARNING: Failed to download Inter font: {exc}")


def fetch_jetbrains_mono() -> None:
    jbm_dir = ASSETS_FONTS_DIR / "jetbrains-mono"
    jbm_dir.mkdir(parents=True, exist_ok=True)
    existing = list(jbm_dir.glob("*.ttf"))
    if existing:
        log(f"JetBrains Mono font already present in {jbm_dir} ({len(existing)} files).")
        return

    log("Fetching JetBrains Mono (Terminal Monospace Font)...")
    url = "https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip"
    try:
        data = download_bytes(url)
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            count = 0
            for name in zf.namelist():
                if name.endswith(".ttf"):
                    filename = Path(name).name
                    if filename:
                        content = zf.read(name)
                        (jbm_dir / filename).write_bytes(content)
                        count += 1
        log(f"JetBrains Mono font installed to {jbm_dir} ({count} files).")
    except Exception as exc:
        log(f"WARNING: Failed to download JetBrains Mono font: {exc}")


def fetch_liberation() -> None:
    serif_dir = ASSETS_FONTS_DIR / "liberation-serif"
    sans_dir = ASSETS_FONTS_DIR / "liberation-sans"
    serif_dir.mkdir(parents=True, exist_ok=True)
    sans_dir.mkdir(parents=True, exist_ok=True)

    serif_existing = list(serif_dir.glob("*.ttf"))
    sans_existing = list(sans_dir.glob("*.ttf"))
    if serif_existing and sans_existing:
        log(f"Liberation fonts already present.")
        return

    log("Fetching Liberation Fonts (Standard Serif & Sans)...")
    url = "https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz"
    try:
        data = download_bytes(url)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            serif_count = 0
            sans_count = 0
            for member in tar.getmembers():
                if member.isfile() and member.name.endswith(".ttf"):
                    filename = Path(member.name).name
                    f = tar.extractfile(member)
                    if f:
                        content = f.read()
                        if "LiberationSerif-" in filename:
                            (serif_dir / filename).write_bytes(content)
                            serif_count += 1
                        elif "LiberationSans-" in filename:
                            (sans_dir / filename).write_bytes(content)
                            sans_count += 1
        log(f"Liberation Serif installed ({serif_count} files), Sans installed ({sans_count} files).")
    except Exception as exc:
        log(f"WARNING: Failed to download Liberation fonts: {exc}")


def main() -> None:
    print("====================================================")
    print("    LCL OS - Font Asset Manager & Downloader        ")
    print("====================================================")
    fetch_inter()
    fetch_jetbrains_mono()
    fetch_liberation()
    print("====================================================")
    print("    LCL Fonts Installation & Setup Complete!        ")
    print("====================================================")


if __name__ == "__main__":
    main()
