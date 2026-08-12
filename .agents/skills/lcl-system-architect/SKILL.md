---
name: lcl-system-architect
description: Provides domain-specific guidance for LCL Core Linux architecture and implementation, including DRM/KMS buffer management, Skia and EGL rendering, evdev input dispatch, and JavaScript engine C bindings. Use when designing, reviewing, or changing these LCL OS subsystems.
---

# LCL System Architect

## Description
Provides domain-specific expertise for building LCL Core Linux components including DRM/KMS buffer management, Skia canvas context initialization, and JS engine C-bindings.

## Capabilities
1. **DRM/KMS Pipeline:** Setting up mode-setting, dumb buffers, and EGL surfaces directly on `/dev/dri/card0`.
2. **Skia Integration:** Creating `SkCanvas` surfaces hooked to EGL OpenGL FBOs.
3. **Event Dispatching:** Converting raw `evdev` struct inputs into high-level LCL UI Events (`LCL_EVENT_MOUSE_MOVE`, `LCL_EVENT_KEY_DOWN`).
4. **JS Bindings:** Exposing C++ methods to JavaScript runtime safely without memory corruption.

## Usage Guidelines
- When generating graphics code, ensure Skia context flushes correctly to EGL surfaces.
- When generating input code, account for non-blocking read calls on input event file descriptors.
