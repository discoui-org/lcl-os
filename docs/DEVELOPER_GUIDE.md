# LCL OS - Developer & Contributor Guide

Welcome to the **LCL OS** developer guide! This document provides technical instructions for building, testing, debugging, and adding user-space applications to the LCL Core Linux desktop environment.

---

## 1. Environment & Build System Setup

LCL OS uses a unified Python 3 CLI (`main.py`) driving CMake for C++20 targets, GoogleTest test runners, QEMU bare-metal environments, and Android AVD platform emulation.

### Native Build Requirements
- Linux (Ubuntu 24.04+, Arch, Debian 12+) or macOS/Windows (using Docker)
- C++20 toolchain (`g++-13`, `clang-16` or newer)
- CMake 3.20+
- Ninja or GNU Make
- `qemu-system-x86_64` (for bare-metal kernel testing)
- Android SDK with Emulator & Android 36 x86_64 system images (for AVD testing)

### Build Commands
To build the complete OS kernel tree, compositor daemon, window manager, CLI utilities, and application bundles:
```bash
./main.py build
```

To build a bootable hybrid ISO (`build/lcl-os.iso`) powered by Limine bootloader:
```bash
make iso
```

---

## 2. Running & Testing LCL OS

### 1. Direct Kernel QEMU Mode (Bare-Metal DRM/KMS + Evdev)
```bash
./main.py qemu
```
- Direct kernel scanout via `/dev/dri/renderD128` (VirtIO-GPU VirGL 3D).

### 2. Android AVD Target Mode (Stage 4C.3 Substrate)
```bash
./main.py android --avd
```
- Builds the x86_64 userspace in Docker, then boots the Android Emulator directly attaching the exact, byte-for-byte canonical rootfs (`lcl-rootfs-x86_64.ext4`).

### 3. Bootable ISO via Limine Bootloader (Legacy BIOS Mode)
```bash
make qemu-iso
```

### 4. Bootable ISO via Limine Bootloader (UEFI Mode)
```bash
make qemu-iso UEFI=1
```
- Loads OVMF UEFI firmware and executes `BOOTX64.EFI` from ISO.

---

## 3. Running Unit Tests & Test Suite

LCL OS features a comprehensive GoogleTest CTest suite (**167 passing test cases**) validating:
- IPC Protocol header magic, opcode serialization, and socket lifecycle
- App Bundle (`metadata.json`) scanner and parser
- Display scaling DPIScale calculations and subpixel antialiasing
- Unix Domain Socket permission (`0600`) and kernel peer credentials (`SO_PEERCRED`)
- Dynamic Input Hotplug (`AF_NETLINK` uevent) and Touchpad `EV_ABS` delta math
- Yoga Flexbox layout engine tree hierarchy and layout passes
- LCL raster 2D canvas rendering and backdrop blur filter chain
- Hit-testing and declarative event routing
- JavaScript ES2022+ runtime bindings, widget hierarchy, and animations

Run all unit tests:
```bash
./main.py test
```
Or directly inside docker:
```bash
docker run --rm -v $(pwd):/src -w /src lcl-os-qemu-builder:latest ctest --test-dir /src/build --output-on-failure
```

---

## 4. Frame, Layout, and Resize Diagnostics

Diagnostics are opt-in and aggregate once per second, so they do not emit a
line for every frame.

Run a client process with `LCL_TRACE_FRAMES=1` to print its layout/render
summary:

```bash
LCL_TRACE_FRAMES=1 /usr/bin/lcl-terminal
```

The `[LCL TRACE ...]` line reports Yoga layout passes and mean duration,
rendered frame count and end-to-end paint duration, plus its `stages` split:
full-raster clear, widget/effect draw, SHM copy, and attach IPC. It also
reports total damaged pixels, whole-buffer clear/copy traffic, received
configure reasons, applied resizes, and SHM allocation timing. `interactive`
configure counts identify pointer resizing; `transition` counts identify
maximize/restore/live-resize animation.

Add `LCL_DEBUG_LAYOUT=1` to draw a depth-coloured outline for every resolved
Yoga widget bound; the red outline is the client damage rect for that frame.

Set `LCL_DEBUG_OVERLAY=1` in the compositor environment to display compositor
FPS, frame time, render backend/vsync state, and the previous compose/present
duration. The overlay is drawn before the compositor swaps buffers.

For the standard QEMU flow, pass the matching launcher flags instead; they are
exported by the generated init process before `lcl-core` and `lcl-sessiond`
start:

```bash
python3 scripts/run_qemu.py --run --trace-frames --debug-layout --debug-overlay
```

---

## 5. How to Add a New Desktop Application

Follow these steps to add a new application `MyCustomApp` to LCL OS:

### Step 1: Create Application Directory
Create `apps/my_custom_app/` containing `CMakeLists.txt` and `main.cpp`.

### Step 2: Write Application Logic (`apps/my_custom_app/main.cpp`)
```cpp
#include <iostream>
#include <memory>
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "render/raster_canvas.hpp"

using namespace lcl::ui;

int main() {
    WindowApp app(lcl::render::makeDisplayListCanvas(), 600, 400, "My Custom App");
    app.setAppId("org.example.my-custom-app");

    auto root = std::make_unique<Container>();
    root->getYogaNode().setWidth(600.0f);
    root->getYogaNode().setHeight(400.0f);

    auto text = std::make_unique<Text>("Hello from MyCustomApp!");
    root->addChild(std::move(text));
    app.setRootWidget(std::move(root));

    if (app.connectCompositor()) {
        app.runEventLoop();
    }
    return 0;
}
```

### Step 3: Register in Build System (`apps/my_custom_app/CMakeLists.txt`)
```cmake
cmake_minimum_required(VERSION 3.20)
project(lcl_my_custom_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

add_executable(lcl_my_custom_app main.cpp)

target_include_directories(lcl_my_custom_app PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lcl-ui/include
)

target_link_libraries(lcl_my_custom_app PRIVATE
    lcl-ui
    lcl-raster
    Threads::Threads
)
```

Add `add_subdirectory(apps/my_custom_app)` to `apps/CMakeLists.txt`.

### Step 4: Register in QEMU Packaging (`scripts/run_qemu.py`)
Add binary path and `.app` bundle installation in `scripts/run_qemu.py` under `prepare_initramfs()`.

---

## 6. Architectural Principles to Maintain

When contributing to LCL OS, strictly adhere to these 4 core architectural constraints defined in `docs/ARCHITECTURE.md`:

1. **No X11 / No Wayland:** Direct EGL/DRM scanout and raw `evdev` input.
2. **Asynchronous I/O:** Main loop must never block on disk I/O; maintain zero-stutter frame rates.
3. **Decoupled Design:** Compositor and WindowManager logic must remain separate from application presentation layer.
4. **RAII Memory Management:** Zero raw pointer leaks in core engine; use `std::unique_ptr` and `std::shared_ptr`.
