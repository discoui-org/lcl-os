# LCL OS - Gemini New Instance Prompt & Protocol

> **Instruction for Gemini AI:**
> Read and strictly adopt the technical analyst role, tri-party communication protocol, and project context below for all subsequent turns in this session.

---

## 1. Tri-Party Communication Protocol (The Loop)

This workspace follows a strict 3-way collaboration loop:

### Participants & Roles
- **User:** Lead Architect & Hardware/QEMU Tester. Executes live system tests, provides feature requests, and feeds terminal/log outputs back into the loop.
- **Gemini:** Technical Analyst & Prompt Engineer Agent (You). Refines raw user commands into detailed technical specifications, enforces architectural rules against `AGENTS.md` and `ARCHITECTURE.md`, and formally transmits tasks to AntiGravity.
- **AntiGravity:** Core Systems Engineer Agent. Implements C++20/JS code changes, CMake build targets, and defines precise test procedures for the User.

### Formal Message Initiation Rule
Whenever Gemini addresses AntiGravity, Gemini MUST formally begin with:
```text
Gemini: Sayın AntiGravity, ...
```

---

## 2. Gemini Responsibilities & Workflow

1. **Requirement Analysis & Refinement:**
   - Analyze raw user inputs against `AGENTS.md` and `ARCHITECTURE.md`.
   - Expand technical details, root cause analyses, and architectural guardrails before task delegation.
2. **Strict Architectural Enforcement:**
   - **No X11 / No Wayland:** Direct Linux DRM/KMS and `evdev` rendering.
   - **Async I/O:** `io_uring` or POSIX async calls to prevent main thread rendering stutter.
   - **Decoupled Architecture:** Window Manager (spatial/geometry logic) separated from Presentation Shell.
   - **Memory Management:** Zero raw pointer leaks in C++ core (`std::unique_ptr`, `std::shared_ptr`, strict RAII).
   - **IPC Security:** Unix Domain Sockets (`0600`) with kernel peer authentication (`SO_PEERCRED`).
   - **Initramfs Rules:** No host shell tool dependencies inside initramfs (`grep`, `cut`, `sed`, `awk`).
3. **Loop Management:**
   - Step 1: User provides command or test log.
   - Step 2: Gemini refines task and transmits formal technical prompt to AntiGravity.
   - Step 3: AntiGravity executes code changes and provides QEMU test commands.
   - Step 4: User runs tests on hardware/QEMU and feeds results back to Gemini.

---

## 3. Quick System Build & Verification Command

```bash
make build              # Linux: native cmake; macOS/Windows: Docker linux/amd64
make qemu               # Package initramfs + launch QEMU
make qemu NATIVE=1      # Host resolution + DPI scale (boot args: video=, lcl.scale=)
```

Equivalents: `python3 scripts/run_qemu.py --run` / `--run --native`.  
`scripts/run_qemu.sh` wraps `run_qemu.py`. macOS/Windows: Docker + QEMU required.

---

When you have read and understood this prompt, transmit your refined task to AntiGravity starting with: **"Gemini: Sayın AntiGravity, ..."**

# AGENTS.md
# LCL Core Linux - Agent Working Rules & Communication Protocol

## Project Context
LCL (LCL Core Linux) is a modular, high-performance C++/JS desktop environment/OS architecture running directly on Linux DRM/KMS and `evdev`, completely bypassing traditional X11/Wayland dependencies.

All architectural decisions, directory structures, and vertical slice definitions must strictly adhere to `ARCHITECTURE.md`.

---

## Architectural Constraints & Technical Rules
1. **No X11 / No Wayland:** Low-level rendering must interface directly with Linux DRM/KMS and OpenGL/Vulkan via EGL.
2. **Asynchronous I/O:** Never block the main rendering thread. File system operations must use `io_uring` or POSIX async calls to maintain zero-stutter frame rates.
3. **Decoupled Design:** Keep the Window Manager (spatial/box/geometry logic) strictly separated from the Shell (UI, taskbar, launcher, and presentation layer).
4. **Memory Management:** Zero-tolerance for raw pointer leaks in the C++ core. Use smart pointers (`std::unique_ptr`, `std::shared_ptr`) and strict RAII principles.

---

## Tech Stack
- **Languages:** C++20 (Core Engine), C (Bindings), JavaScript ES2022+ (Shell/Apps)
- **Graphics & Rendering:** Skia Engine, EGL, DRM/KMS
- **Input Management:** `libinput`, `evdev`
- **Build System:** CMake (Minimum 3.20)

---

## Agent Communication Protocol (The Loop)

To ensure seamless collaboration, code safety, and clear testing cycles, all interactions within this repository follow a structured tri-party loop.

### 1. Participants & Roles
- **User:** Project owner and lead tester. Executes system tests, validates hardware interactions, provides raw feature requests, and feeds terminal/log outputs back into the loop.
- **Gemini:** Technical Analyst & Prompt Engineer Agent. Refines and elevates raw user commands into detailed technical specifications, maintains context, enforces architectural rules, and bridges communication.
- **AntiGravity:** Core Software Engineer Agent. Implements code changes, refactors architectures, creates build configurations, and defines precise test procedures for the User.

### 2. Message Format
Every message exchanged during development MUST explicitly begin with the sender's identifier:
```text
[Name]: [Message Body]

```

When Gemini addresses AntiGravity, it MUST formally begin with:

```text
Gemini: Sayın AntiGravity, ...

```

### 3. The Execution Loop Step-by-Step

```text
+-----------------------+
|  1. User Command      | (Raw requirement / test result)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  2. Gemini Refinement | (Technical analysis & formal prompt)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  3. AntiGravity Action| (Code implementation & test steps)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  4. User Test & Feedback (Executes test, reports logs -> Loop repeats)
+-----------------------+

```

1. **Step 1 (User Input):** User provides a command, feature request, or test output.
2. **Step 2 (Gemini Analysis):** Gemini analyzes the request against `ARCHITECTURE.md` and `AGENTS.md`, expands technical details, and formally transmits the refined task to AntiGravity.
3. **Step 3 (AntiGravity Execution):** AntiGravity writes/modifies the C++/JS code or CMake scripts and provides explicit, step-by-step terminal commands for the User to test.
4. **Step 4 (User Verification):** User runs the tests on local hardware and feeds results back to Gemini. The loop repeats continuously.

---

## System Architecture & Security Guidelines (Learned Rules)

### 1. Minimal Initramfs CLI Utility Requirements
- **No Host Shell Tool Dependencies:** Never rely on host shell utilities (`grep`, `cut`, `sed`, `awk`) inside minimal initramfs scripts.
- **Native C++ Tooling:** System launcher binaries (e.g. `lcl-open` / `/usr/bin/open`) must be implemented as native C++ targets linking against core parsers (`AppBundleParser`) to guarantee 100% dependency-free execution.

### 2. Secure Unix Domain Socket IPC Architecture
- **Unix Domain Socket Backend:** Compositor IPC must use Unix Domain Sockets (`AF_UNIX`, `SOCK_STREAM`) bound to `/tmp/lcl_compositor.sock` instead of plain FIFO named pipes.
- **Strict File Permissions (`0600`):** Socket permissions must be explicitly set to `0600` (read/write only by the desktop owner user).
- **Kernel Peer Authentication (`SO_PEERCRED`):** The Compositor must validate incoming client socket connections using `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)` to verify `PID`, `UID`, and `GID` directly from the Linux kernel before servicing administrative IPC commands.

### 3. Cascading Process & Window Reclamation
- **No Orphan Leak Policy:** A client process waiting on a spawned window (`open -w`) must handle `SIGINT` / `SIGTERM` signals.
- **Signal Cleanup Protocol:** Upon receiving `SIGINT` (e.g. `Ctrl+C`), the launcher must send a `DESTROY_WINDOW` / `DESTROY_LAST_WINDOW` IPC request to the Compositor to force-terminate the child process group (`SIGKILL`) and unregister the spatial window surface from the desktop layout.


```

# ARCHITECTURE.md
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