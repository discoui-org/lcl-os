# LCL Core Linux (LCL OS)

> **Modular, High-Performance Linux Desktop Architecture & Display Server**
> *Direct Linux DRM/KMS & evdev (No X11 / No Wayland dependencies)*

---

## Overview

**LCL OS** is a lightweight, high-performance C++20 desktop environment and display server built directly on top of the Linux Kernel DRM/KMS subsystem and `evdev` input layer. By removing traditional display server overheads (X11/Wayland), LCL OS achieves zero-stutter frame rates, hardware VSync synchronization, zero-latency cursor planes, and dynamic high-refresh-rate pacing (60Hz–250Hz+).

---

## Architectural Highlights

- **Direct DRM/KMS & EGL Backend:** Interfacing directly with `/dev/dri/card0` and `/dev/dri/renderD128` via Mesa EGL/GBM (VirGL 3D GPU acceleration or `llvmpipe` CPU fallback).
- **Skia 2D Rendering & Vector Typography:** TrueType vector font engine with macOS-style antialiasing and subpixel rendering.
- **Dynamic Refresh Rate Pacing & Headroom Allowance:** Hardware-agnostic monitor refresh rate detection with a ~0.5ms safety headroom allowance ($\sim 155\text{ FPS}$ preparation budget for 144Hz displays) guaranteeing zero VBlank misses and zero frame skip.
- **Zero-Resource Idle State:** Non-busy blocking state when the desktop canvas is idle, dropping CPU utilization to $\sim 0\%$ and displaying `FPS: 0 (Idle)`.
- **Decoupled `lcl-ui` Application Framework:** Yoga Flexbox layout engine, polymorphic widget hierarchy (`Container`, `Button`, `Text`), damage rect tracking, and custom 2D canvas drawing.
- **Secure Unix Domain Socket IPC:** Unix Domain Socket (`/tmp/lcl_compositor.sock`) bound with strict `0600` permissions, Linux Kernel peer authentication (`SO_PEERCRED`), and zero-copy shared memory (`memfd`).

---

## Project Structure

```text
lcl-os/
├── src/                      # Core OS Engine & Compositor
│   ├── core/                 # DRM/KMS, EGL, Evdev Input, IPC, Session
│   ├── render/               # Skia Renderer, FontRenderer, WindowManager
│   └── tools/                # Core System Daemons & Binaries (lcl-core, lcl-desktop-wm, lcl-terminal, lcl-open)
├── lcl-ui/                   # Decoupled UI Application Framework (Yoga Flexbox, Widget Tree)
├── apps/                     # User-Space Desktop Applications
│   ├── ui_demo/              # Flexbox Interactive Widget Demo App (UIDemo.app)
│   └── shader_demo/          # 144Hz Procedural Shader & Animation App (ShaderDemo.app)
├── docs/                     # Technical Documentation & Guides
│   ├── ARCHITECTURE.md       # Low-level system & architectural design
│   ├── LCL_UI_FRAMEWORK.md   # Complete lcl-ui developer API guide
│   └── DEVELOPER_GUIDE.md    # Build, testing, and contribution instructions
└── scripts/                  # QEMU Isolated Boot Launcher & Packaging
```

---

## Quick Start & Building

### Prerequisites
- Linux host or Docker (the build system automatically uses an isolated Docker container `lcl-os-qemu-builder` if local toolchain is absent).
- CMake 3.20+ and C++20 compiler (`g++-13` or `clang-16`+).
- QEMU (`qemu-system-x86_64`) for running the direct kernel boot environment.

### Building
Compile all core binaries, libraries, unit tests, and application bundles:
```bash
make
```

### Running in QEMU Virtual Machine

#### 1. Hardware VirGL 3D Mode (144Hz GPU Acceleration)
```bash
make qemu GPU=1 NATIVE=1
```

#### 2. Software Raster Mode (Mesa `llvmpipe` CPU)
```bash
make qemu GPU=0 NATIVE=1
```

---

## Running Applications Inside LCL OS

When LCL OS boots in QEMU, launch applications from the built-in terminal or using the `open` command:

- **Launch 144Hz Procedural Shader Demo:**
  ```bash
  lcl_shader_demo
  # or
  open ShaderDemo.app
  ```
- **Launch Interactive Flexbox UI Demo:**
  ```bash
  lcl_ui_demo
  # or
  open UIDemo.app
  ```
- **Launch Additional Terminal Instances:**
  ```bash
  open Terminal.app
  ```

---

## Diagnostic Overlay & HUD

LCL OS includes a real-time diagnostic HUD in the top-right corner displaying:
- **FPS & Frame Time:** Rolling 1.0s sliding window (`FPS: 144 (6.9 ms)` or `FPS: 0 (Idle)` when canvas is static).
- **Render Engine:** Active Mesa driver probed via GL audit (`Engine: VirGL 3D (GPU)` or `Engine: Mesa llvmpipe (CPU)`).
- **VSync Status:** Hardware EGL VSync synchronization state (`VSync: ON`).

---

## Running Unit Tests

Run the full GoogleTest CTest suite (19 passing unit tests covering IPC protocol, bundle parsing, display scaling, Yoga layout, and event routing):
```bash
make test
```

---

## Documentation

- [Architecture Specification](docs/ARCHITECTURE.md)
- [lcl-ui Framework Developer Guide](docs/LCL_UI_FRAMEWORK.md)
- [Developer & Testing Guide](docs/DEVELOPER_GUIDE.md)

---

## License

Copyright © 2026 LCL OS Team. All rights reserved.
