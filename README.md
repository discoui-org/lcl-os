# LCL Core Linux (LCL OS)

Modular, Lightweight Linux Distribution built with C++20 and Limine bootloader. Custom Linux distribution architecture built from scratch with a focus on modularity, high performance, and platform-independent userspace binaries.

LCL OS runs directly on bare-metal Linux DRM/KMS and `evdev` (as well as Android Composer AIDL/HIDL substrates), completely bypassing traditional X11 and Wayland display server dependencies.

---

## Project Structure

```text
lcl-os/
├── frameworks/               # Graphics, motion, theme, UI and window chrome libraries
├── system/                   # Compositor, scene, input, IPC, session, shells and raster service
├── platforms/                # Common contracts plus Linux and Android substrates
├── apps/                     # Built-in applications
├── tooling/                  # Build, deploy, emulator, generator, probe and asset tools
├── packaging/iso/            # Limine boot configuration and ISO overlay
├── config/                   # Gestalt and device profiles
├── assets/                   # Fonts, icons and source wallpapers
├── tests/                    # Framework, system, platform, integration and tooling tests
├── docs/                     # Technical documentation and guides
└── out/                      # Generated builds, images, caches and tool downloads (ignored)
```

Generated artifacts use one canonical layout defined by `tooling/paths.py`:

```text
out/host
out/qemu/{x86_64,aarch64}
out/android/{x86_64,aarch64}
out/rootfs
out/images/{android,iso}
out/{utm,cache,tools,skia,visual-baselines}
```

Legacy `build*` directories remain ignored local artifacts and are not migrated
or accepted as validation of this layout.

---

## Prebuilt Skia SDK packages

LCL does not require developers to compile Skia locally. Run the
**Build Skia SDK packages** workflow once from GitHub Actions. It builds and
publishes checksum-verified packages for:

- `host-x86_64`
- `host-aarch64`
- `android-x86_64`
- `android-arm64-v8a`

The normal Python build entry points automatically download the exact package
matching the pinned Skia revision and LCL build recipe into
`out/skia/packages/<target>`. To prefetch every ABI explicitly:

```bash
python3 tooling/build/fetch_skia.py --all
```

Set `LCL_SKIA_GITHUB_REPOSITORY=owner/repository` when consuming packages from
a fork. For a private repository, pass a GitHub token with read access; an
authenticated GitHub CLI can supply it without storing it in the project:

```bash
GITHUB_TOKEN="$(gh auth token)" python3 tooling/build/fetch_skia.py --all
```

Direct CMake configuration can use the downloaded package with
`-DLCL_SKIA_ROOT=out/skia/packages/<target>`.

---

## Quick Start & Unified CLI

LCL OS provides a unified CLI driver via `./main.py`:

```bash
./main.py --help
```

### 1. Run the Full Unit Test Suite
```bash
./main.py test
```
*Executes the current GoogleTest/CTest suite covering IPC protocols, window management, LCL raster rendering, event routing, and JS runtimes.*

### 2. Build Core System & Canonical RootFS
```bash
./main.py build
```
*Compiles C++20 engine binaries, builds application bundles, and produces the canonical userspace image `out/rootfs/lcl-rootfs-x86_64.ext4`.*

### 3. Run in QEMU Bare-Metal Environment (DRM/KMS + Evdev)
```bash
./main.py qemu
```
*Launches direct kernel boot with VirtIO GPU hardware acceleration.*

### 4. Run in Android AVD Target (Stage 4C.3 Substrate)
```bash
./main.py android --avd
```
*Builds the x86_64 userspace in Docker, then boots the Android Emulator directly attaching the exact, byte-for-byte canonical rootfs (`lcl-rootfs-x86_64.ext4`).*

### 5. Run on a Rooted ARM64 Android Phone (Composer3 AIDL / Composer 2.2–2.4 HIDL)

```bash
./main.py android
```

This builds the ARM64 Android substrate and canonical rootfs, deploys both, then
starts the full LCL desktop session. Later runs can reuse existing artifacts:

```bash
./main.py android --no-build
```

Use `--rebuild` for a clean compositor rebuild, `--push-rootfs` to force a
rootfs upload, `--compositor-only` for substrate diagnostics, or
`--restore-only` to recover Android UI after an interrupted session. The first
build downloads pinned official AOSP VNDK headers and pulls compatible HIDL
libraries from the connected phone without modifying it. It also builds a
pinned ARM64 zstd helper, so rootfs deployment does not depend on optional
Android system utilities. Deployment temporarily
stops SurfaceFlinger for exclusive Composer access and restores Android UI when
the LCL process exits. Root access is required.

To deploy the existing x86_64 artifacts to an already-running, root-capable
Android Emulator without generating or booting custom AVD images, use the same
ADB deployment command:

```bash
./main.py android --no-build
```

The deployer reads `ro.product.cpu.abi`: `x86_64` selects `out/android/x86_64/` and
`lcl-rootfs-x86_64.ext4`, while `arm64-v8a` selects the physical-device ARM64
artifacts. Root may come from an already-root adbd, `adb root`, or `su -c`.

---

## Running Applications Inside LCL OS

When LCL OS boots, launch additional Terminal instances using the `open` command:

- **Launch an Additional Terminal Instance:**
  ```bash
  open Terminal.app
  ```

---

## Key Architectural Principles

1. **No X11 / No Wayland:** Direct EGL/DRM/KMS scanout on bare metal; native Composer3 AIDL or Composer 2.2–2.4 HIDL on mobile substrates.
2. **Platform-Independent Application Binaries (Same-Binary Invariant):** For the same CPU architecture, LCL application executables (`Terminal.app`, `lcl-desktop-shell`, `lcl-sessiond`, etc.) are 100% byte-for-byte identical across Linux DRM/KMS and Android AVD targets.
3. **Retained Presentation:** Clients atomically commit ready immutable layers; Window Manager owns spatial geometry, while the compositor retains the last ready layer and animates/composes it independently of application rendering.
4. **Secure Unix Domain Socket IPC:** Robust little-endian protocol over `SOCK_SEQPACKET` with `0600` permissions and kernel peer credential verification (`SO_PEERCRED`).

---

## Documentation

- [Architecture Specification](docs/ARCHITECTURE.md)
- [lcl-ui Framework Developer Guide](docs/LCL_UI_FRAMEWORK.md)
- [Developer & Testing Guide](docs/DEVELOPER_GUIDE.md)

---

## License

MIT License
