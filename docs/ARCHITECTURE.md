# LCL Core Linux (LCL)

> **LCL**: LCL Core Linux (Recursive Abbreviation)

LCL is a modular, layered graphics operating system architecture designed to run directly on top of Linux kernel capabilities without the overhead of traditional desktop environments (heavy UI toolkits over X11/Wayland). It combines the raw performance of **C/C++** with the flexibility of a **JavaScript** runtime.

---

## 1. Architectural Theme & Vertical Slice

The LCL architecture consists of 4 main layers from bottom to top. Each layer is designed to be completely decoupled.

```text
+-------------------------------------------------------+
|  Top Layer / UI (LCL Shell & Apps - JS)               |
+-------------------------------------------------------+
|  Bridge & Runtime (C Binding / JS Engine)             |
+-------------------------------------------------------+
|  Graphics & Compositor Engine (Skia & C/C++ Manager)  |
+-------------------------------------------------------+
|  Linux Kernel / Hardware Layer (DRM/KMS/evdev)        |
+-------------------------------------------------------+

```

---

## 2. Layer Details & Technology Stack

### I. Hardware & Input Layer

* **Location:** `src/core/`
* **Language:** C / C++
* **Hardware Access (DRM/KMS):** Directly renders images onto the GPU framebuffer using Linux Kernel's `Direct Rendering Manager` (DRM) and `Kernel Mode Setting` (KMS) subsystems, bypassing X Server or traditional display managers.
* **Input Management (`libinput` / `evdev`):** Directly captures keyboard, mouse, and touchscreen events from `/dev/input/` device nodes and dispatches them to the event queue.

### II. Rendering Engine & Compositor Layer

* **Location:** `src/render/`
* **Language:** C++
* **Graphics Engine (Skia):** Uses Google's open-source 2D graphics engine (Skia) for rendering vector graphics, text, shadows, and managing GPU-accelerated OpenGL/Vulkan outputs.
* **Window Manager:** A C++ engine that calculates window bounds (`width, height`), spatial positions (`x, y`), layer depths (`z-index`), and focus states.
* **Decoupling Principle:** The Window Manager strictly handles geometric and physical window rules; it contains no visual Shell elements.

### III. Bridge & Runtime Layer

* **Location:** `src/binding/`
* **Language:** C++ / C / JavaScript
* **JS Engine:** Lightweight JavaScript runtime (QuickJS / V8) for executing user-space code.
* **C/JS Binding:** Maps C++ Skia drawing commands and system hardware events (mouse/keyboard events) into JavaScript objects and functions.

### IV. Shell & User Interface Layer

* **Location:** `shell/`
* **Language:** JavaScript / TypeScript
* **LCL Shell:** The user-facing visual surface containing the taskbar, application launcher, wallpaper, and notification area. Decoupled from the Window Manager.
* **Application Ecosystem:** Designed to support React Native / declarative JS paradigms in future stages via integration with `Yoga Layout` (C++ Flexbox engine).

---

## 3. Subsystems

### A. Asynchronous File System & Storage

* **Location:** `src/fs/`
* **Architecture:** Asynchronous I/O architecture running over the Linux Kernel VFS (Virtual File System).
* **I/O Engine:** Leverages modern Linux `io_uring` and standard POSIX APIs for zero-cost, non-blocking disk operations that prevent main render loop stutters.
* **File Watching:** Live directory tracking using `inotify`.
* **Security & Sandboxing:** Application directory isolation exposed via the `LCL.fs` JavaScript API.
* **Disk Partitioning Strategy (Immutable Layout):**
* `/system`: Read-only operating system and Shell core files.
* `/home/user`: Writable user data and configurations.
* `/apps`: Isolated application packages.



---

## 4. Event & Data Flow Architecture

When an event occurs (e.g., a user mouse click), the vertical execution flow operates as follows:

1. **Kernel (`evdev`)** $\rightarrow$ Captures raw hardware event (`/dev/input/`).
2. **C++ Window Manager** $\rightarrow$ Calculates coordinate hit-testing and identifies target window.
3. **C Binding Layer** $\rightarrow$ Translates the event into a JS `onClick` object and dispatches it to the JS Runtime.
4. **JS Shell / Application** $\rightarrow$ Updates application state and triggers a re-render signal back to the C++ layer.
5. **Skia Engine** $\rightarrow$ Renders the updated frame directly to the screen via GPU/DRM.

---

## 5. Directory Structure

```text
lcl-os/
├── ARCHITECTURE.md         # Architecture documentation
├── CMakeLists.txt          # Root CMake configuration
├── third_party/            # External dependencies (Skia, QuickJS/V8, etc.)
├── build/                  # Build output directory
├── shell/                  # Top Layer (JavaScript)
│   ├── assets/             # Fonts, wallpapers, icons
│   └── js/
│       └── main.js         # LCL Shell entry point
└── src/                    # Lower Layer (C/C++)
    ├── main.cpp            # Application entry point & Event Loop
    ├── core/
    │   ├── display/        # DRM/KMS, OpenGL/Vulkan framebuffer management
    │   └── input/          # evdev & libinput event listeners
    ├── render/             # Skia window rendering & compositing engine
    ├── fs/                 # io_uring / POSIX async file system
    └── binding/            # C++ <-> JS communication bridge

```