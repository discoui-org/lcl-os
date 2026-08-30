#!/usr/bin/env python3
"""Render LCL icon SVGs to PNG using the repository-local icon font."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def parse_args() -> argparse.Namespace:
    root = repository_root()
    parser = argparse.ArgumentParser(
        description="Convert assets/icons/*.svg to PNG with CupertinoIcons.ttf."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=root / "assets" / "icons",
        help="Directory containing the SVG icon sources.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root / "assets" / "icons" / "png",
        help="Directory for generated PNG files.",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=1024,
        help="Square output size in pixels (default: 1024).",
    )
    return parser.parse_args()


def require_command(name: str) -> str:
    command = shutil.which(name)
    if command is None:
        raise RuntimeError(f"Required command is not available: {name}")
    return command


def fontconfig_document(font_dir: Path, cache_dir: Path) -> str:
    return f"""<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>{escape(str(font_dir))}</dir>
  <cachedir>{escape(str(cache_dir))}</cachedir>
</fontconfig>
"""


def verify_local_font(env: dict[str, str], expected_font: Path) -> None:
    fc_match = require_command("fc-match")
    result = subprocess.run(
        [fc_match, "--format=%{file}", "CupertinoIcons"],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    matched = Path(result.stdout.strip())
    if not matched.exists() or not os.path.samefile(matched, expected_font):
        raise RuntimeError(
            "Fontconfig did not resolve CupertinoIcons to the repository-local "
            f"font (resolved: {matched})."
        )


def verify_png(path: Path, expected_size: int) -> None:
    with path.open("rb") as image:
        header = image.read(24)
    if len(header) != 24 or header[:8] != PNG_SIGNATURE:
        raise RuntimeError(f"Renderer did not create a valid PNG: {path}")
    width, height = struct.unpack(">II", header[16:24])
    if (width, height) != (expected_size, expected_size):
        raise RuntimeError(
            f"Unexpected PNG size for {path.name}: {width}x{height}, "
            f"expected {expected_size}x{expected_size}."
        )


def main() -> int:
    args = parse_args()
    if args.size <= 0:
        raise RuntimeError("--size must be a positive integer.")

    input_dir = args.input_dir.resolve()
    output_dir = args.output_dir.resolve()
    root = repository_root()
    font_file = root / "assets" / "fonts" / "cupertino-icons" / "CupertinoIcons.ttf"
    if not font_file.is_file():
        raise RuntimeError(f"Local icon font is missing: {font_file}")

    svg_files = sorted(input_dir.glob("*.svg"))
    if not svg_files:
        raise RuntimeError(f"No SVG icons found in {input_dir}")

    rsvg_convert = require_command("rsvg-convert")
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="lcl-icon-fontconfig-") as temp_path:
        temp_dir = Path(temp_path)
        config_file = temp_dir / "fonts.conf"
        config_file.write_text(
            fontconfig_document(font_file.parent, temp_dir / "cache"),
            encoding="utf-8",
        )
        env = os.environ.copy()
        env["FONTCONFIG_FILE"] = str(config_file)
        verify_local_font(env, font_file)

        for svg_file in svg_files:
            png_file = output_dir / f"{svg_file.stem}.png"
            subprocess.run(
                [
                    rsvg_convert,
                    "--width",
                    str(args.size),
                    "--height",
                    str(args.size),
                    "--keep-aspect-ratio",
                    "--output",
                    str(png_file),
                    str(svg_file),
                ],
                check=True,
                env=env,
            )
            verify_png(png_file, args.size)
            print(f"rendered {svg_file.name} -> {png_file.name}")

    print(f"converted {len(svg_files)} icons at {args.size}x{args.size}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
