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
- **Leo:** Technical Analyst & Prompt Engineer Agent. Refines and elevates raw user commands into detailed technical specifications, maintains context, enforces architectural rules, and bridges communication.
- **Katrina:** Core Software Engineer Agent. Implements code changes, refactors architectures, creates build configurations, and defines precise test procedures for the User.

### 2. Message Format
Every message exchanged during development MUST explicitly begin with the sender's identifier:
```text
[Name]: [Message Body]

```

When Leo addresses Katrina, it MUST formally begin with:

```text
Leo: Sayın Katrina, ...

```

### 3. The Execution Loop Step-by-Step

```text
+-----------------------+
|  1. User Command      | (Raw requirement / test result)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  2. Leo Refinement | (Technical analysis & formal prompt)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  3. Katrina Action| (Code implementation & test steps)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  4. User Test & Feedback (Executes test, reports logs -> Loop repeats)
+-----------------------+

```

1. **Step 1 (User Input):** User provides a command, feature request, or test output.
2. **Step 2 (Leo Analysis):** Leo analyzes the request against `ARCHITECTURE.md` and `AGENTS.md`, expands technical details, and formally transmits the refined task to Katrina.
3. **Step 3 (Katrina Execution):** Katrina writes/modifies the C++/JS code or CMake scripts and provides explicit, step-by-step terminal commands for the User to test.
4. **Step 4 (User Verification):** User runs the tests on local hardware and feeds results back to Leo. The loop repeats continuously.

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