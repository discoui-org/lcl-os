---
name: lcl-system-architect
description: Provides domain-specific guidance for LCL Core Linux architecture and implementation, including DRM/KMS buffer management, Android AIDL Composer3 substrate, logical display lists, GLES/software raster replay, evdev input dispatch, and JavaScript engine C bindings. Use when designing, reviewing, or changing these LCL OS subsystems.
---

# LCL System Architect

## Description
Provides domain-specific expertise for building LCL Core Linux components including DRM/KMS buffer management, Android AIDL Composer3 multi-platform substrates, backend-neutral logical display lists, GLES/software raster replay, and JS engine C-bindings.

## Capabilities
1. **DRM/KMS Pipeline:** Setting up mode-setting, dumb buffers, and EGL surfaces directly on `/dev/dri/card0` / `/dev/dri/renderD128`.
2. **Android Substrate Pipeline (Stage 4C.3):** Managing Android platform composition server (`lcl-core-android`), AIDL Composer3 / HWComposer integration, and mounting the canonical rootfs (`/mnt/lcl`) with isolated devpts (`/mnt/lcl/dev/pts`) and `/Runtime` IPC bridges.
3. **Graphics/Raster Boundary:** Keeping `lcl-graphics` display lists in logical units while a producer-side `lcl-raster` stage replays them through GLES or software, applies `RenderTarget.deviceScale`, and publishes an immutable ready layer before atomic commit.
4. **Event Dispatching:** Converting raw `evdev` struct inputs into high-level LCL UI Events (`LCL_EVENT_MOUSE_MOVE`, `LCL_EVENT_KEY_DOWN`).
5. **JS Bindings:** Exposing C++ methods to JavaScript runtime safely without memory corruption.

## Usage Guidelines
- When generating graphics code, keep widgets and chrome backend-neutral, record through `lcl::graphics::Canvas`, and let the raster backend own EGL/GLES details and device scaling.
- Keep application DisplayList replay in supervised central `lcl-rasterd`, outside the compositor presentation loop. Protocol v27 grants each surface a 128-bit producer capability; only rasterd's private channel may publish `LayerReady`. The compositor retains the last committed layer and animates presentation state independently; a brand-new surface remains unmapped until its first layer is ready.
- Treat resize as one non-selectable `AtomicRetained` WindowGroup transaction. Parent, size-changing attachments, and size-changing popups share one generation; never serialize them parent-first, stretch old content, reveal fill, crossfade snapshots, or timeout into a partial group. Keep at most one generation rastering and coalesce newer pointer targets behind it so refresh-cadenced cancellation cannot starve a slow group.
- When generating input code, account for non-blocking read calls on input event file descriptors.
- Enforce the Same-Binary Invariant: platform differences terminate strictly below the LCL userspace ABI.
