# LCL Core Linux (LCL) Architecture Specification

> **LCL**: LCL Core Linux (Recursive Abbreviation)

LCL is a modular, layered graphics operating system architecture running directly on bare-metal Linux kernel capabilities (`DRM/KMS`, `evdev`, `io_uring`, `AF_UNIX`) without traditional X11 or Wayland display server dependencies. It combines high-performance C++20 engine primitives with native desktop window management and JavaScript/native application runtime capabilities.

---

## 1. Architectural Layers & Vertical Slice

The LCL architecture consists of 4 main decoupled layers:

```text
+-----------------------------------------------------------------------+
|  Top Layer / Apps & Shell (JS Shell, LCL Terminal, System Monitor)     |
+-----------------------------------------------------------------------+
|  Application & IPC Layer (AppBundleParser, lcl-open, AF_UNIX IPC)     |
+-----------------------------------------------------------------------+
|  Graphics & Window Manager (Skia/stb_truetype, FontRenderer, PTY)     |
+-----------------------------------------------------------------------+
|  Linux Kernel & Hardware Layer (DRM/KMS, virtio_gpu, evdev, io_uring) |
+-----------------------------------------------------------------------+
```

---

## 2. Layer Details & Subsystems

### I. Hardware & Input Layer
* **Location:** `src/core/display/`, `src/core/input/`
* **Language:** C++20 / C
* **Display Management (DRM/KMS):** Directly drives GPU framebuffers via Linux DRM/KMS (`/dev/dri/card0`, `/dev/dri/renderD128`) with `virtio_gpu` VirGL hardware acceleration support.
* **Input Subsystem (`evdev` / `libinput`):** Captures hardware keyboard, mouse, and touch events directly from `/dev/input/event*` nodes and dispatches them to the desktop event queue.

### II. Rendering & Window Management Layer
* **Location:** `src/render/`, `src/apps/`
* **Language:** C++20
* **Font & Text Engine (`FontRenderer`):** TrueType vector font rasterization via `stb_truetype` featuring subpixel antialiasing, macOS-style gamma correction, font-agnostic metric queries (`getCellWidth()`, `getCellHeight()`), and UTF-8 multi-byte sequence handling.
* **Window Manager:** Decoupled spatial window layout engine tracking z-index, spatial coordinates (`x, y, width, height`), focus, and window frame rendering.
* **Terminal Engine (`TerminalApp` & `PTYManager`):** Pseudo-terminal (`/dev/pts/`) controller spawning interactive GNU Bash shells. Features font-agnostic canvas layout, `TIOCSWINSZ` PTY window size synchronization, VT100 write-head overwrite state tracking, and typing-aware 500ms blinking inverted block cursor rendering.

### III. IPC & Application Bundle Subsystem
* **Location:** `src/core/ipc/`, `src/tools/`
* **Secure Unix Domain Socket IPC:** Compositor IPC operating over Unix Domain Sockets (`/tmp/lcl_compositor.sock`) bound with strict `0600` permissions. Validates client requests using kernel peer authentication (`SO_PEERCRED` via `getsockopt`) to verify `PID`, `UID`, and `GID`.
* **Native App Bundle Architecture (`.app`):** macOS-style `.app` bundles containing `metadata.json`, `bin/`, and `assets/`.
* **System Launcher (`lcl-open` / `/usr/bin/open`):** Native C++ CLI tool linking against `AppBundleParser`. Supports background launch (`open App.app`) and blocking wait mode (`open -w App.app`) with signal handling (`SIGINT`/`SIGTERM`) and automatic window surface reclamation (`DESTROY_LAST_WINDOW`).

### IV. Storage & Asynchronous File System
* **Location:** `src/fs/`
* **Architecture:** Non-blocking asynchronous I/O over Linux Kernel VFS using `io_uring` and POSIX async primitives to prevent main rendering thread stutters.

---

## 3. Communication & Event Flow Architecture

```text
+-----------------------+
|  1. Hardware Event    | (Keyboard / Mouse event via /dev/input/event*)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  2. Input Subsystem   | (evdev / libinput event listener)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  3. Window Manager    | (Hit-testing, focus dispatch, spatial bounds)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  4. PTY / App Handler | (Writes sequence to master FD / IPC Socket)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  5. Renderer Engine   | (FontRenderer & DRM/KMS buffer swap)
+-----------------------+
```

---

## 4. Directory Structure

```text
lcl-os/
├── Makefile                        # make build | qemu | qemu NATIVE=1
├── CMakeLists.txt                  # Root CMake build configuration
├── assets/                         # System fonts and visual assets
│   └── fonts/                      # TrueType font assets (JetBrains Mono, Inter)
├── docs/                           # Documentation & Agent prompts
│   ├── ARCHITECTURE.md             # System architecture specification
│   ├── AG_NEW_INSTANCE_PROMPT.md   # AntiGravity instance bootstrapper prompt
│   └── GEMINI_NEW_INSTANCE_PROMPT.md # Gemini instance bootstrapper prompt
├── scripts/                        # System build & QEMU launcher
│   ├── run_qemu.py                 # Cross-platform QEMU launcher (Linux/macOS/Windows)
│   ├── run_qemu.sh                 # Thin wrapper → run_qemu.py
│   ├── Dockerfile.qemu             # linux/amd64 builder image (macOS/Windows)
│   └── fetch_fonts.sh              # Font asset fetcher
├── shell/                          # Shell Presentation Layer
└── src/                            # Core C++20 Engine & Applications
    ├── main.cpp                    # Application entry point & compositor loop
    ├── apps/                       # Native system applications
    │   ├── sysmon/                 # System Monitor application
    │   └── terminal/               # LCL Terminal application & VT100 engine
    ├── core/                       # Core engine subsystems
    │   ├── display/                # DRM/KMS & OpenGL/Vulkan display backend
    │   ├── input/                  # evdev & libinput event listeners
    │   ├── ipc/                    # Secure Unix Domain Socket IPC server
    │   └── terminal/               # PTY master/slave manager
    ├── render/                     # Renderer engine & stb_truetype FontRenderer
    ├── fs/                         # io_uring & POSIX async file system
    └── tools/                      # Native CLI utilities (lcl-open)
```

### Build & QEMU (dev hosts)

| Host | Build | Run |
|------|--------|-----|
| Linux | Native `cmake` (`make build`) | `make qemu` |
| macOS / Windows | Docker `linux/amd64` (`Dockerfile.qemu`) | Host QEMU + packaged kernel/initramfs |

```bash
make qemu            # default 1280x800, host refresh rate
make qemu NATIVE=1   # host logical resolution + lcl.scale (kernel cmdline)
```

Guest display boot args (when `NATIVE=1`):
- HiDPI/Retina: `video=` = **physical** pixels, `lcl.scale` = DPR (sharp UI; avoids zoom-upscale blur)
- 1x displays: `video=` = host resolution, `lcl.scale=1`
- QEMU cocoa: `full-screen=on,zoom-to-fit=on` (fill screen if mode list is inexact)

`DisplayManager` prefers boot-requested mode. `DisplayScale` multiplies fonts/chrome/windows by `lcl.scale`.