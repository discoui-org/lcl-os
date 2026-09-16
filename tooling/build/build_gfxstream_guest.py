#!/usr/bin/env python3
"""Build the pinned, headless gfxstream guest Vulkan ICD for an LCL rootfs.

The small LCL patch is applied to an out-of-tree source overlay, never to the
pinned submodule.  It replaces gfxstream's virtio guest transport with the
already capability-authorized LCL socket IOStream; Vulkan opcodes remain
upstream gfxstream's responsibility.
"""

from __future__ import annotations

import argparse
import hashlib
import platform
import subprocess
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
GFXSTREAM_ROOT = PROJECT_ROOT / "third_party" / "gfxstream"
EXPECTED_COMMIT = "6247388dea0ce60c1dcb9cfc3572c05e0e912697"
LCL_PATCH = PROJECT_ROOT / "frameworks" / "gfxstream" / "patches" / "0001-lcl-socket-iostream.patch"


def normalize_arch(value: str | None) -> str:
    candidate = (value or platform.machine()).lower().strip()
    return "aarch64" if candidate in {"aarch64", "arm64", "armv8", "armv9"} else "x86_64"


def pinned_commit() -> str:
    if not (GFXSTREAM_ROOT / ".git").exists():
        raise RuntimeError("third_party/gfxstream is not an initialized git submodule")
    return subprocess.check_output(
        ["git", "-C", str(GFXSTREAM_ROOT), "rev-parse", "HEAD"], text=True
    ).strip()


def prepare_overlay(arch: str) -> Path:
    patch_key = hashlib.sha256(LCL_PATCH.read_bytes()).hexdigest()[:16]
    overlay = PROJECT_ROOT / "out" / "gfxstream" / normalize_arch(arch) / f"source-{patch_key}"
    if overlay.exists():
        try:
            head = subprocess.check_output(
                ["git", "-C", str(overlay), "rev-parse", "HEAD"], text=True
            ).strip()
        except subprocess.CalledProcessError:
            # Build overlays are linked Git worktrees.  A workspace moved from
            # a container path (for example /src) retains an old gitdir link;
            # repair it in place instead of modifying the pinned checkout or
            # silently deleting cached build inputs.
            subprocess.run(
                ["git", "-C", str(GFXSTREAM_ROOT), "worktree", "repair", str(overlay)],
                check=True,
            )
            head = subprocess.check_output(
                ["git", "-C", str(overlay), "rev-parse", "HEAD"], text=True
            ).strip()
        if head != EXPECTED_COMMIT:
            raise RuntimeError("gfxstream build overlay does not match the approved pin")
    else:
        overlay.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-C", str(GFXSTREAM_ROOT), "worktree", "add", "--detach",
                        str(overlay), EXPECTED_COMMIT], check=True)
    marker = overlay / ".lcl-socket-transport-patch"
    if not marker.is_file():
        subprocess.run(["git", "-C", str(overlay), "apply", "--check", str(LCL_PATCH)], check=True)
        subprocess.run(["git", "-C", str(overlay), "apply", str(LCL_PATCH)], check=True)
        marker.write_text(patch_key + "\n", encoding="utf-8")
    return overlay


def build(arch: str, lcl_adapter: Path | None = None) -> Path:
    actual = pinned_commit()
    if actual != EXPECTED_COMMIT:
        raise RuntimeError(
            "gfxstream source is not the approved pin: "
            f"expected {EXPECTED_COMMIT}, got {actual}"
        )

    if lcl_adapter is None or not lcl_adapter.is_file():
        raise RuntimeError("The Android-flavour LCL gfxstream guest adapter archive is required")
    lcl_build_root = lcl_adapter.parents[2]
    lcl_archives = [
        lcl_adapter,
        lcl_build_root / "frameworks" / "gpu" / "liblcl-gpu-client.a",
        lcl_build_root / "frameworks" / "client" / "liblcl-client.a",
        lcl_build_root / "system" / "liblcl-gpu-protocol.a",
        lcl_build_root / "system" / "liblcl-protocol.a",
        lcl_build_root / "system" / "liblcl-raster-protocol.a",
    ]
    missing_archives = [str(path) for path in lcl_archives if not path.is_file()]
    if missing_archives:
        raise RuntimeError("Missing LCL guest transport link archive(s): " + ", ".join(missing_archives))
    guest_link_args = ["-Wl,--whole-archive", str(lcl_adapter), "-Wl,--no-whole-archive",
                       *(str(path) for path in lcl_archives[1:])]
    source_dir = prepare_overlay(arch)
    build_dir = source_dir.parent / f"guest-{source_dir.name.removeprefix('source-')}"
    artifact = build_dir / "guest" / "vulkan" / "libvulkan_gfxstream.so"
    options = [
        "-Dgfxstream-build=guest",
        "-Dplatforms=[]",
        "-Degl-native-platform=surfaceless",
        "-Dgallium-drivers=[]",
        "-Dvulkan-drivers=gfxstream-experimental",
        "-Dbuild-tests=false",
        "-Dbuildtype=release",
        "-Dlcl-socket-transport=true",
        "-Dlcl-guest-link-args=[" + ",".join(repr(arg) for arg in guest_link_args) + "]",
    ]
    if (build_dir / "meson-private" / "coredata.dat").is_file():
        command = ["meson", "setup", "--reconfigure", str(build_dir), str(source_dir), *options]
    else:
        command = ["meson", "setup", str(build_dir), str(source_dir), *options]
    subprocess.run(command, check=True)
    subprocess.run(
        ["ninja", "-C", str(build_dir), "guest/vulkan/libvulkan_gfxstream.so"],
        check=True,
    )
    if not artifact.is_file():
        raise RuntimeError(f"gfxstream guest build did not produce {artifact}")
    return artifact


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default=None, help="Target architecture (x86_64 or aarch64)")
    parser.add_argument("--lcl-adapter", type=Path, required=True,
                        help="Static Android-flavour LCL guest transport adapter")
    args = parser.parse_args()
    artifact = build(normalize_arch(args.arch), args.lcl_adapter)
    print(artifact)


if __name__ == "__main__":
    main()
