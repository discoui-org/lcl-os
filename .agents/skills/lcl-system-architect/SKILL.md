---
name: lcl-system-architect
description: Provides domain-specific guidance for LCL Core Linux architecture and implementation, including DRM/KMS buffer management, Android AIDL Composer3 substrate, Skia and EGL rendering, evdev input dispatch, and JavaScript engine C bindings. Use when designing, reviewing, or changing these LCL OS subsystems.
---

# LCL System Architect

## Description
Provides domain-specific expertise for building LCL Core Linux components including DRM/KMS buffer management, Android AIDL Composer3 multi-platform substrates, Skia canvas context initialization, and JS engine C-bindings.

## Capabilities
1. **DRM/KMS Pipeline:** Setting up mode-setting, dumb buffers, and EGL surfaces directly on `/dev/dri/card0` / `/dev/dri/renderD128`.
2. **Android Substrate Pipeline (Stage 4C.3):** Managing Android platform composition server (`lcl-core-android`), AIDL Composer3 / HWComposer integration, and mounting the canonical rootfs (`/mnt/lcl`) with isolated devpts (`/mnt/lcl/dev/pts`) and `/Runtime` IPC bridges.
3. **Skia Integration:** Creating `SkCanvas` surfaces hooked to EGL OpenGL FBOs and shared memory (memfd) client buffers.
4. **Event Dispatching:** Converting raw `evdev` struct inputs into high-level LCL UI Events (`LCL_EVENT_MOUSE_MOVE`, `LCL_EVENT_KEY_DOWN`).
5. **JS Bindings:** Exposing C++ methods to JavaScript runtime safely without memory corruption.

## Usage Guidelines
- When generating graphics code, ensure Skia context flushes correctly to EGL surfaces.
- When generating input code, account for non-blocking read calls on input event file descriptors.
- Enforce the Same-Binary Invariant: platform differences terminate strictly below the LCL userspace ABI.
