# App / Scene / Shell Refactor Baseline

This document freezes the behavioral and verification baseline for the app,
scene, and shell refactor. Later steps must preserve this baseline unless a
change is explicitly approved before implementation.

## Baseline anchor

- Git commit: `a44a46b898a2ffd28d81acd3bb6240e5c7c30b44`
- Branch: `master`
- Working tree at capture: clean
- Capture date: 2026-08-12

Automated checks captured at this commit:

| Check | Command | Result |
| --- | --- | --- |
| Host build | `cmake --build build_host -j2` | Passed; all default targets built |
| Unit/integration tests | `ctest --test-dir build_host --output-on-failure` | Passed; 58/58 tests, 0.32 s |
| Patch whitespace | `git diff --check` | Passed |
| QEMU package/smoke | `python3 scripts/run_qemu.py --package-only --native --gpu` | Not runnable in the capture environment because Docker was unavailable |

The QEMU row is a required manual gate, not an accepted failure. It must be
completed on the normal development host before Step 2 begins.

## Change classification

The following behavior is part of the baseline and must remain:

- A client surface is not mapped as a window before its first valid SHM buffer
  commit.
- Unknown-surface buffer attachments are rejected and their received FDs are
  closed.
- Replacing a `WindowApp` root schedules a real frame and an initial SHM attach
  can be retried without leaving the surface permanently unmapped.
- Wallpaper and shell panels start without app-window enter transitions, do not
  steal application focus, and do not receive ordinary application input.
- Terminal rendering stays in `TerminalView` under `WindowApp`; the previous
  handwritten terminal socket/frame renderer is not restored.
- Widgets draw through the backend-neutral `Canvas` contract.
- CSD and SSD use the shared window-chrome geometry and window-action contract.

These parts are temporary compatibility bridges and must be removed only after
the revisioned app/scene state pipeline has equivalent tests:

- Protocol v2 and its legacy payload-size compatibility branches. Removed in
  Step 3 after the v3 codec and rejection tests became the only accepted path.
- PID/executable-path based `appId` inference.
- Full `WindowListUpdate` snapshots and per-client snapshot hashes.
- Dock-side application-directory rescans.
- Rebuilding the complete Dock root with `setRootWidget()` for every window
  list update.
- Client-declared privileged shell roles.
- Process launch paths split between `lcl-open`, compositor, and
  `lcl-desktop-wm`. Removed in Step 4; `lcl-sessiond` is the only application
  process owner.
- EGL/DRM/GBM/GLES and renderer sources compiled directly into `lcl-ui`.
- The old `Renderer`/VGA path and the current combined client/compositor
  `SkiaRenderer` implementation.

## Behavioral contract

### Terminal

- The terminal uses client-side decoration with the shared titlebar layout.
- Titlebar controls have the same geometry and antialiasing as the SSD controls.
- The terminal root and backdrop remain passive: hovering or clicking the
  window background must not brighten the entire window like a button.
- Backdrop blur/glass and translucency remain visible over the wallpaper.
- Terminal text uses the packaged JetBrains Mono face.
- Typing must not blink, stall, or wait for an unrelated shell-state update.
- Continuous resize must not make the surface opaque, lose its effect graph, or
  introduce alternating old/new buffers.

### Desktop shell

- Wallpaper fills the display and remains visible during shell startup.
- Menu bar is visible at the top edge.
- Dock is visible at the bottom edge after its first valid buffer commit.
- Menu, Dock, and wallpaper remain unfocusable and never replace the focused
  application.
- System surfaces never appear as ordinary application windows.
- A shell surface may be recreated without depending on another receiver's
  previous window-list hash.

### Lifecycle and input

- Client EOF and explicit close release the surface buffer/FD and remove its
  window after the closing transition.
- No placeholder window or fallback terminal text is drawn before a real
  client buffer exists.
- CSD drag/minimize/maximize/restore/close requests continue to use compositor
  owned window actions.
- Logical input and layout coordinates remain independent from physical SHM
  dimensions through `bufferScale`.

## QEMU acceptance gate

Use a fixed display configuration so later measurements are comparable:

```sh
python3 scripts/run_qemu.py --run --width 1920 --height 1080 --scale 1 --gpu
```

Record the compositor refresh rate and renderer backend from the boot log. The
comparison is valid only when those values and the QEMU GPU mode match.

Verify in this order:

1. Wallpaper, menu bar, Dock, and one Terminal are all visible after boot.
2. Terminal focus remains active after the shell panels map.
3. Type a continuous line for at least five seconds; characters must appear in
   order with no visible blink or shell-induced pause.
4. Resize the Terminal continuously for at least five seconds; glass,
   translucency, titlebar, and text must remain stable.
5. Open UI Demo, switch focus between it and Terminal, then close UI Demo; the
   focused titlebar and Dock indicator must follow the active application.
6. Close Terminal and confirm that its surface is removed without leaving a
   placeholder or stale Dock entry.

Save one screenshot after boot and one while Terminal overlaps a high-contrast
wallpaper area. Those images are the visual references for all later steps.

## Step gate

No subsequent refactor step may proceed until:

- the automated build and full CTest suite still pass;
- the fixed-resolution QEMU checklist above is accepted;
- typing and continuous resize show no regression;
- any intentional visual difference is approved before its implementation.

## Step 2 implementation record

Step 2 separates build ownership without changing protocol or shell behavior:

- `lcl-ui` now contains only widgets, Yoga layout, event/render-pass logic,
  `WindowApp`, image loading, and IPC/SHM client lifecycle.
- `lcl-display-scale` owns the shared logical-pixel policy.
- `lcl-canvas-skia` owns the client software raster Canvas and font renderer;
  it is compiled with all EGL/OpenGL branches disabled.
- `lcl-render` owns compositor rendering, window management, EGL, DRM, GBM,
  GLES, and presentation.
- `WindowApp` requires an injected Canvas. Native clients and the JS binding
  explicitly inject `makeSkiaCanvas()`.

The Step 2 automated gate must include archive/link inspection proving that
`liblcl-ui.a`, Terminal, desktop shell, and JS runtime have no EGL/DRM/GBM/GLES
symbols or link dependencies. The compositor must continue to link those
libraries through `lcl-render`.

## Step 3 implementation record

Step 3 replaces the compositor/client transport without changing shell-state
ownership or widget/render behavior:

- Protocol v3 is the only accepted wire version. Every header and payload field
  is encoded and decoded explicitly in little-endian order.
- The compositor socket is an owner-only Unix `SOCK_SEQPACKET` endpoint at
  `/run/user/1000/lcl-compositor.sock`; stream and text-command fallbacks are
  removed.
- Client requests carry non-zero monotonic request IDs. The compositor returns
  typed `AckResponse` packets using the corresponding request ID.
- Non-blocking sends retain packet order in a bounded per-socket queue. Queued
  `SCM_RIGHTS` descriptors are duplicated and released when sent or discarded.
- `bufferScale` is mandatory, finite, and restricted to `0.5..4.0`; logical
  bounds/input remain separate from physical SHM dimensions.
- Packet length, opcode, enum, string, float, effect-graph, ancillary-data, and
  FD/opcode combinations are validated before dispatch. Only `AttachBuffer`
  may carry one descriptor.
- `WindowApp`, compositor dispatch, desktop shell, JS runtime users, and the
  temporary desktop-WM client use the same v3 transport in this step.

The Step 3 automated gate is the full default build plus all CTest tests,
including explicit little-endian encoding, v2 rejection, required scale,
truncated packet, FD attachment, request-ID, and queued-order cases. The QEMU
check remains the behavioral acceptance gate before Step 4.

## Step 4 implementation record

Step 4 separates application lifecycle authority from the compositor without
changing compositor surface, focus, or shell-state ownership:

- `lcl-sessiond` owns the cached `.app` registry, manifest-backed canonical
  `id`, default Terminal profile, process instance ID, child reaping, and
  process-exit notification.
- Session RPC is a distinct owner-only Unix `SOCK_SEQPACKET` protocol at
  `/run/user/1000/lcl-sessiond.sock`; it supports typed catalog snapshot,
  launch response, and wait-for-exit flow with explicit little-endian fields.
- `lcl-open` is now a session client and no longer parses bundles, forks, or
  execs applications. `--wait` waits for the session-owned process exit event.
- The compositor no longer links `StartupManager`, app-bundle parsing, terminal
  app, or PTY launch code. The temporary desktop-WM compatibility client also
  no longer spawns a Terminal.
- QEMU init starts compositor, then sessiond, then the desktop shell. Sessiond
  launches the default Terminal once.

The desktop shell's icon lookup and its legacy `WindowListUpdate` consumer are
deliberately retained until Steps 5 and 6 provide the revisioned scene/focus
broker and typed shell client. They are read-only compatibility paths, not
application launch authority.

## Step 5 implementation record

Step 5 introduces compositor-owned scene and focus authority without changing
desktop-shell presentation or the existing `WindowListUpdate` compatibility
transport:

- `SceneRegistry` creates a stable `SceneId` only when a client application
  surface first maps from a valid buffer. It records surface/window identity,
  app ID, client PID, title, geometry, display/workspace placeholders, and
  visible/minimized/closing lifecycle state. Wallpaper and shell-panel
  surfaces never become scenes.
- `FocusController` derives the active scene for seat 0 from the
  `WindowManager` focus result. It performs no hit testing, z-ordering, or
  window mutation.
- `ShellStateBroker` owns a monotonic revision and bounded replayable deltas,
  with snapshot fallback when a consumer is behind retained history. It owns
  no socket or UI; the typed subscription and shell client belong to Step 6.
- Mapping, client EOF, explicit close, close-animation completion, and
  compositor input/resize/minimize/focus changes all reconcile into the same
  scene/focus stream. `SurfaceRegistry` remains SHM/FD owner and
  `WindowManager` remains geometry/focus executor.
- The current Dock/menu are intentionally unchanged. Their legacy window-list
  input remains a read-only compatibility path until Step 6 consumes the
  broker through a typed client.
