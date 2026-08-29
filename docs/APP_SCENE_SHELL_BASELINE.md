# App / Scene / Shell Refactor Baseline

This document freezes the behavioral and verification baseline for the app,
scene, and shell refactor. Later steps must preserve this baseline unless a
change is explicitly approved before implementation.

The numbered implementation records below are historical checkpoints and keep
the protocol versions and test totals that were true at each checkpoint. The
current wire contract is protocol v27. Central `lcl-rasterd` receives sealed
logical frames and privately publishes ready immutable layers; app-facing
DisplayList and native-buffer attach messages have been removed. Do not read an
older step's version label as a compatibility promise or future direction.

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

- A client surface is not mapped as a window before its first complete,
  presentable layer commit.
- Unknown or mismatched producer grants/layers are rejected and their received
  FDs are closed.
- Replacing a `WindowApp` root schedules a real frame; the complete retained
  frame/resource state is retried after rasterd restart without mapping an
  empty surface.
- Wallpaper and shell panels start without app-window enter transitions, do not
  steal application focus, and do not receive ordinary application input.
- Terminal rendering stays in `TerminalView` under `WindowApp`; the previous
  handwritten terminal socket/frame renderer is not restored.
- Widgets draw through the backend-neutral `Canvas` contract.
- CSD and DesktopWM frames host the same renderer-independent `WindowChromeWidget` for
  geometry, hit testing, actions, interaction state, motion, and visual colors.

The following compatibility bridges have been removed after their replacement
paths received equivalent revisioned-state coverage:

- Protocol v2 and its legacy payload-size compatibility branches in Step 3.
- PID/executable-path based `appId` inference, full `WindowListUpdate`
  snapshots, Dock-side rescans/root replacement, and client-declared roles in
  Step 9.
- Process launch paths split between `lcl-open`, compositor, and the obsolete
  `lcl-desktop-wm` helper in Step 4/9; `lcl-sessiond` is the only application
  process owner.
- EGL/DRM/GBM/GLES and renderer sources compiled directly into `lcl-ui`.
- The old `Renderer`/VGA path and the current combined client/compositor
  `RasterRenderer` implementation.

## Behavioral contract

### Terminal

- The terminal uses DesktopWM-owned frame controls with edge-to-edge material;
  its outer-surface backdrop material extends beneath the titlebar.
- The terminal client buffer is content-only. Titlebar layout, hit testing,
  actions, motion, and painting stay in DesktopWM's attached frame surface;
  compositor policy sees only a generic parent-child surface relationship.
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
- No blank placeholder window or fallback terminal text is drawn before a real
  client layer exists. A shell-owned launch icon/proxy may animate without
  mapping an empty application surface.
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

- `lcl-ui` contains widgets, its private layout implementation, event/render-pass logic,
  `WindowApp`, image loading, compositor IPC, and the rasterd producer client.
- `lcl-display-scale` owns the shared logical-pixel policy.
- `lcl-raster` owns the Canvas implementation. A connected protocol-v27 client
  uses `makeDisplayListCanvas()` and sends only sealed logical frames/resources
  to central `lcl-rasterd`; it neither attaches a client buffer to the
  compositor nor presents a frame. The current host rasterd backend publishes
  immutable memfd/SHM layers. DMA-BUF and AHardwareBuffer remain platform
  backend work below this same private raster-service contract.
- `lcl-render` owns compositor rendering, window management, EGL, DRM, GBM,
  GLES, and presentation.
- `WindowApp` requires an injected Canvas. Native clients and the JS binding
  explicitly inject `makeDisplayListCanvas()` for compositor-connected
  surfaces.

The boundary gate proves that `liblcl-ui.a` has no EGL/DRM/GBM/GLES dependency.
KMS scanout and display presentation remain exclusively compositor/platform
responsibilities; rasterd native allocation/import backends stay below the
private layer-ready boundary.

## Step 3 implementation record

Step 3 replaces the compositor/client transport without changing shell-state
ownership or widget/render behavior:

- Protocol v4 is the only accepted wire version. Every header and payload field
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
- Protocol v4 adds a monotonic `configureSerial` to `ConfigureBounds` and
  `AttachBuffer`; stale or dimension-mismatched resize buffers are rejected.
- Packet length, opcode, enum, string, float, effect-graph, ancillary-data, and
  FD/opcode combinations are validated before dispatch. Only `AttachBuffer`
  may carry one descriptor.
- `WindowApp`, compositor dispatch, desktop shell, JS runtime users, and the
  temporary desktop-WM client use the same v4 transport in this step.

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
- At the end of Step 5, the current Dock/menu were intentionally unchanged and
  their legacy window-list input remained a read-only compatibility path. Step
  6 replaces that active consumer with the typed broker client below.

## Step 6 implementation record

Step 6 replaces the desktop shell's active window-list compatibility path with
a typed, revision-aware client without changing Dock/menu visual treatment:

- Protocol v3 added `SubscribeShellState`, `ShellStateSnapshot`, and
  `ShellStateDelta`. Snapshot and delta payloads are explicitly little-endian,
  carry `revision`, `seatId`, `displayId`, `workspaceId`, and stable scene IDs,
  and encode no compositor renderer or SHM resources.
- `ProtocolDispatcher` sends an initial snapshot to each subscribed shell
  receiver and then replays bounded broker deltas. A revision older than
  retained history (or ahead of current authority) receives a fresh snapshot.
- `lcl-shell-state` is a widget-independent typed client. It detects a delta
  gap and requests a snapshot rather than applying guessed state. This is the
  same client boundary a future mobile shell can use.
- Desktop Dock now projects scenes by canonical `appId` into one running entry
  per application, derives its active indicator from `FocusChanged`, and keeps
  the clean-profile Terminal pin. It no longer reads `WindowListUpdate`, scans
  application directories, or replaces its root widget for every update.
- Icon metadata comes from the cached sessiond catalog. Sessiond resolves a
  manifest-relative icon once before publishing catalog data; shell UI never
  rescans `.app` bundles.
- `WindowApp` sends a declared canonical app ID in `SurfaceCreate`; Terminal
  and native UI Demo now use their manifest IDs. PID/executable inference stays
  only as compatibility for clients not yet migrated to declaration.

## Step 7 implementation record

Step 7 makes Menu Bar and Dock compositor-recognised system surfaces while
preserving their visual and non-interactive baseline:

- `SetSystemSurfaceKind` declares `Wallpaper`, `MenuBar`, or `Dock` before a
  surface is created. The compositor verifies the declaring peer is the
  desktop-shell executable, then applies one `SystemSurfacePolicy` for role,
  layer, focusability, decoration, inset border, and transition behavior.
- Normal clients cannot self-promote to `ShellPanel`/`DesktopWallpaper`; those
  privileged registrations are accepted only from the trusted shell peer.
  Typed shell-state subscription retains its trusted `ShellPanel` capability.
- Menu and Dock no longer send `SetWindowLayer` or `SetReservedZone`. Once a
  real buffer maps, compositor derives the top/bottom reserved work area from
  their system-surface kind and committed dimensions. A system client cannot
  override that policy with later layer/reserved-zone commands.
- System surfaces stay outside `SceneRegistry`, remain unfocusable, and retain
  their no-enter-transition behavior. Dock still does not receive pointer
  input; application activation UI is deliberately a later product step.

## Step 8 implementation record

Step 8 makes the wallpaper's output geometry compositor-owned without changing
its asset or visual treatment:

- Wallpaper's system-surface policy now carries `OutputBounds` placement. At
  `SurfaceCreate`, compositor replaces client-requested x/y/width/height with
  its active output bounds before the first valid buffer can map the surface.
- The desktop shell still supplies a boot-time buffer size from the kernel
  display configuration, but that value is now only a bootstrap allocation;
  it does not establish wallpaper placement authority.
- Menu and Dock retain their existing client-requested geometry in this step.
  Their layer, focusability, transitions, and reserved-work-area policy remain
  the Step 7 compositor-owned behavior.

## Step 9 implementation record

Step 9 removes the last active shell-state compatibility paths and freezes the
desktop/mobile consumption boundary without adding a mobile UI:

- `WindowListUpdate`, protocol role registration, and the unused
  `lcl-desktop-wm` target are removed. Trusted shell-state subscriptions and
  system-surface declarations are authorised directly by the compositor's
  trusted-shell peer check.
- Every `SurfaceCreate` now carries a non-empty canonical `appId`; compositor
  no longer derives identity from PID or executable paths. Shell system
  surfaces use the desktop-shell identity but remain outside `SceneRegistry`.
- `ShellStateModel` materialises one revisioned snapshot/delta stream without
  widgets. Desktop Dock continues to apply its app grouping above that model;
  `recents()` exposes one non-closing scene per item for a future mobile shell.
