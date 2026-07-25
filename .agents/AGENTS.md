# LCL Core Linux - Agent Working Rules

## Project Context
LCL (LCL Core Linux) is a modular, high-performance C++/JS desktop environment/OS architecture running directly on Linux DRM/KMS and evdev, without X11/Wayland dependencies.

## Architecture Constraints
1. **No X11 / No Wayland:** Low-level rendering must interface directly with Linux DRM/KMS and OpenGL/Vulkan via EGL.
2. **Asynchronous I/O:** Never block the main rendering thread. File system operations must use `io_uring` or POSIX async calls.
3. **Decoupled Design:** Keep Window Manager (spatial/box logic) strictly separated from Shell (UI/taskbar presentation layer).
4. **Memory Management:** Zero-tolerance for raw pointer leaks in C++ core. Use smart pointers (`std::unique_ptr`, `std::shared_ptr`) and RAII principles.

## Tech Stack
- **Languages:** C++20 (Core Engine), C (Bindings), JavaScript ES2022+ (Shell/Apps)
- **Graphics:** Skia Engine, EGL, DRM/KMS
- **Input:** libinput, evdev
- **Build System:** CMake (Minimum 3.20)

Also ensure to follow `ARCHITECTURE.md` for the architecture decisions.