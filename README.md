# LCL Core Linux (LCL OS)

Modular, Lightweight Linux Distribution built with C++20 and Limine bootloader. Custom Linux distribution architecture built from scratch with a focus on modularity, high performance, and platform-independent userspace binaries.

LCL OS runs directly on bare-metal Linux DRM/KMS and `evdev` (as well as Android Composer AIDL/HIDL substrates), completely bypassing traditional X11 and Wayland display server dependencies.

---

## Project Structure

```text
lcl-os/
├── iso_root/                 # Limine bootloader configuration & boot tree
├── src/                      # Core OS Engine & Compositor
│   ├── core/                 # DRM/KMS, EGL, Evdev Input, Hotplug, IPC, Session
│   ├── render/               # LCL raster Renderer, FontRenderer, WindowManager
│   └── tools/                # Core System Daemons & Binaries (lcl-core, lcl-terminal, lcl-open)
├── lcl-ui/                   # Decoupled UI Application Framework (Yoga Flexbox, Widget Tree)
├── apps/                     # User-Space Desktop Applications (Terminal.app, UIDemo.app, ShaderDemo.app)
├── docs/                     # Technical Documentation & Guides
│   ├── ARCHITECTURE.md       # Low-level system & architectural design
│   ├── LCL_UI_FRAMEWORK.md   # Complete lcl-ui developer API guide
│   └── DEVELOPER_GUIDE.md    # Build, testing, and contribution instructions
└── scripts/                  # Unified CLI launcher, ISO Builder, Android Image Builder & Packaging
```

---

## Quick Start & Unified CLI

LCL OS provides a unified CLI driver via `./main.py`:

```bash
./main.py --help
```

### 1. Run the Full Unit Test Suite (167 Tests)
```bash
./main.py test
```
*Executes all 167 GoogleTest CTest cases covering IPC protocols, window management, LCL raster rendering, event routing, and JS runtimes in ~1.7s.*

### 2. Build Core System & Canonical RootFS
```bash
./main.py build
```
*Compiles C++20 engine binaries, builds application bundles, and produces the canonical userspace image `build/rootfs/lcl-rootfs-x86_64.ext4`.*

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

---

## Running Applications Inside LCL OS

When LCL OS boots, launch applications from the built-in terminal or using the `open` command:

- **Launch Interactive Flexbox UI Demo:**
  ```bash
  open UIDemo.app
  ```
- **Launch Procedural Shader Animation Demo:**
  ```bash
  open ShaderDemo.app
  ```
- **Launch Additional Terminal Instances:**
  ```bash
  open Terminal.app
  ```

---

## Key Architectural Principles

1. **No X11 / No Wayland:** Direct EGL/DRM/KMS scanout on bare metal; native Composer3 AIDL or Composer 2.2–2.4 HIDL on mobile substrates.
2. **Platform-Independent Application Binaries (Same-Binary Invariant):** For the same CPU architecture, LCL application executables (`Terminal.app`, `lcl-desktop-shell`, `lcl-sessiond`, etc.) are 100% byte-for-byte identical across Linux DRM/KMS and Android AVD targets.
3. **Decoupled Window Manager & Compositor:** Window Manager owns spatial coordinates and geometry state; Compositor acts as a pure presentation engine.
4. **Secure Unix Domain Socket IPC:** Robust little-endian protocol over `SOCK_SEQPACKET` with `0600` permissions and kernel peer credential verification (`SO_PEERCRED`).

---

## Documentation

- [Architecture Specification](docs/ARCHITECTURE.md)
- [lcl-ui Framework Developer Guide](docs/LCL_UI_FRAMEWORK.md)
- [Developer & Testing Guide](docs/DEVELOPER_GUIDE.md)

---

## License

MIT License
