# LCL Core Linux - Agent Working Rules & Communication Protocol

## Project Context
LCL (LCL Core Linux) is a modular, high-performance C++/JS desktop environment/OS architecture running directly on Linux DRM/KMS and `evdev` (or Android HAL/Composer3 substrate), completely bypassing traditional X11/Wayland dependencies.

All architectural decisions, directory structures, and vertical slice definitions must strictly adhere to `ARCHITECTURE.md`.

---

## Architectural Constraints & Technical Rules
1. **No X11 / No Wayland:** Low-level rendering must interface directly with Linux DRM/KMS and OpenGL/Vulkan via EGL, or platform composition substrates (e.g. Android Composer3).
2. **Asynchronous I/O:** Never block the main rendering thread. File system operations must use `io_uring` or POSIX async calls to maintain zero-stutter frame rates.
3. **Decoupled Design:** Keep the Window Manager (spatial/box/geometry logic) strictly separated from the Shell (UI, taskbar, launcher, and presentation layer).
4. **Memory Management:** Zero-tolerance for raw pointer leaks in the C++ core. Use smart pointers (`std::unique_ptr`, `std::shared_ptr`) and strict RAII principles.
5. **Platform-Independent Application Binaries (Same-Binary Invariant):** For the same CPU architecture, LCL application executables (`lcl-terminal`, `lcl-desktop-shell`, `lcl-sessiond`, `UIDemo.app`, etc.) are platform-independent artifacts and must be reused byte-for-byte across LCL platform targets. Platform differences terminate strictly below the LCL userspace ABI. Alternately compiled or Android-specific application rebuilds are strictly forbidden.
6. **Retained Presentation Boundary:** Application and shell clients produce a complete immutable layer before atomic commit. The compositor retains the last ready layer and owns presentation-tree transforms, effects, composition, and vSync. It must never run application DisplayList replay, layout, font work, or other unbounded client raster work in the presentation-critical loop. A new toplevel is not mapped before its first ready layer; a late client cannot stall compositor-owned motion.

---

## Tech Stack
- **Languages:** C++20 (Core Engine), C (Bindings), JavaScript ES2022+ (Shell/Apps)
- **Graphics & Rendering:** `lcl-graphics` logical display lists, `lcl-raster` GLES/software replay, EGL, DRM/KMS, Android AIDL Composer3
- **Input Management:** `libinput`, `evdev`
- **Build System:** CMake (Minimum 3.20), Python 3 CLI (`main.py`)

---

## Agent Communication Protocol (The Loop)

To ensure seamless collaboration, code safety, and clear testing cycles, all interactions within this repository follow a structured tri-party loop.

### 1. Participants & Roles
- **User / Berkeley:** Project owner and lead tester. Executes system tests, validates hardware interactions, provides raw feature requests, and feeds terminal/log outputs back into the loop.
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
|  2. Leo Refinement    | (Technical analysis & formal prompt)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  3. Katrina Action    | (Code implementation & test steps)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  4. User Verification | (Executes test, reports logs -> Loop repeats)
+-----------------------+
```

1. **Step 1 (User Input):** User provides a command, feature request, or test output.
2. **Step 2 (Leo Analysis):** Leo analyzes the request against `ARCHITECTURE.md` and `AGENTS.md`, expands technical details, and formally transmits the refined task to Katrina.
3. **Step 3 (Katrina Execution):** Katrina writes/modifies the C++/JS code or CMake scripts and provides explicit, step-by-step terminal commands for the User to test.
4. **Step 4 (User Verification):** User runs the tests on local hardware and feeds results back to Leo. The loop repeats continuously.

---

## System Architecture & Security Guidelines (Learned Rules)

### 1. Minimal Initramfs & Substrate Requirements
- **No Host Shell Tool Dependencies:** Never rely on host shell utilities (`grep`, `cut`, `sed`, `awk`, `seq`) inside minimal initramfs or substrate init scripts. Use Bash builtin loops `for ((i=0; i<N; i++))`.
- **Native C++ Tooling:** System launcher binaries (e.g. `lcl-open` / `/usr/bin/open`) must be implemented as native C++ targets linking against core parsers (`AppBundleParser`) to guarantee 100% dependency-free execution.

### 2. Secure Unix Domain Socket IPC Architecture
- **Unix Domain Socket Backend:** Compositor IPC uses Unix Domain `SOCK_SEQPACKET` at `/run/user/1000/lcl-compositor.sock` (or `/Runtime/lcl-compositor.sock` under multi-platform substrates). Protocol v27 keeps each explicit little-endian header and payload in one packet; app-facing layer descriptors and DisplayList commits are forbidden. Central `lcl-rasterd` receives sealed producer memfds on its owner-only socket and publishes `LayerReady` descriptors only through a private compositor socketpair.
- **Strict File Permissions (`0600`):** Socket permissions must be explicitly set to `0600` (read/write only by the desktop owner user).
- **Kernel Peer Authentication (`SO_PEERCRED`):** The Compositor must validate incoming client socket connections using `getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len)` to verify `PID`, `UID`, and `GID` directly from the Linux kernel before servicing administrative IPC commands.

### 3. Cascading Process & Window Reclamation
- **No Orphan Leak Policy:** A client process waiting on a spawned window (`open -w`) must handle `SIGINT` / `SIGTERM` signals.
- **Signal Cleanup Protocol:** Upon receiving `SIGINT` (e.g. `Ctrl+C`), the launcher must send a `DESTROY_WINDOW` / `DESTROY_LAST_WINDOW` IPC request to the Compositor to force-terminate the child process group (`SIGKILL`) and unregister the spatial window surface from the desktop layout.

### 4. Stage 4C.3 Android Substrate & Canonical Userspace Invariants
- **Platform Substrate Boundary:** The Android system image contains ONLY platform-specific substrate components (`lcl-core-android`, `lcl.rc`, `lcl-bootstrap.sh`). Zero user applications or desktop daemons exist inside `system.img`.
- **Exact Canonical RootFS Consumption:** The exact byte-for-byte artifact `out/rootfs/lcl-rootfs-x86_64.ext4` is attached as a secondary block device (`/dev/block/vdf`) and mounted on `/mnt/lcl`.
- **Idempotent Mounts & Isolated Probing:** Device discovery probes MUST use an isolated temporary mountpoint (`/mnt/lcl-probe`) and unmount after probing. If `/mnt/lcl/System/Core/lcl-sessiond` is already present, bootstrap must reuse the existing mount to prevent `EBUSY` and stacked overlays.
- **Kernel devpts Propagation:** Standard `mount --bind /dev /mnt/lcl/dev` does not propagate child mounts. Bootstrap MUST explicitly execute `mount --bind /dev/pts /mnt/lcl/dev/pts` so that glibc `posix_openpt()` can allocate pseudo-terminals for `Terminal.app` without `ENODEV` errors.
- **Shared `/Runtime` tmpfs IPC Bridge:** `/Runtime` tmpfs is bind-mounted to `/mnt/lcl/Runtime`, allowing `lcl-core-android` on the substrate and `lcl-sessiond`/`lcl-desktop-shell`/apps inside the canonical chroot to communicate over identical Unix domain sockets.
