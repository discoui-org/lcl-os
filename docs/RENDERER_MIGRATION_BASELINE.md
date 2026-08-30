# Renderer Migration Baseline

This document is the acceptance anchor for replacing LCL's custom raster
execution with Skia. It does not introduce a second UI framework or a parallel
production renderer.

This file also records a historical cross-process DisplayList checkpoint. That
checkpoint is useful for visual comparison, but compositor-side application
DisplayList replay is no longer the current architecture. Protocol v27 and
central `lcl-rasterd` now implement the normative boundary from
`ARCHITECTURE.md`: rasterd creates an immutable ready layer, atomic commit
publishes it, and the compositor retains and presents it without running client
raster work in its vSync-critical loop.

## Architectural boundary

| Layer | Migration contract |
| --- | --- |
| `lcl-ui` | Keeps `WindowApp -> Widget -> LCL Layout -> Canvas`; layout-engine and Skia types do not enter widget APIs. |
| `lcl-graphics` | Keeps logical geometry and the bounded DisplayList authoring contract inside the layer producer. DisplayList wire terminates at rasterd and is not part of the compositor surface ABI. |
| Raster execution | Moves behind the producer-side Canvas/FrameTransport boundary and outputs an immutable DMA-BUF/native-buffer layer or SHM fallback before commit. |
| Text | Measurement and drawing must use one packaged-font implementation on Android and DRM/KMS targets. |
| Effects | `EffectRegion` and `FilterOp`, including Glass, remain LCL protocol semantics; compositor execution may map them to Skia filters and trusted SkSL. |
| Compositor | Retains scene, last-ready layers, presentation state, damage, pacing, window policy, backdrop capture, composition, and present ownership. It does not replay application DisplayLists on the presentation thread. |
| Platform | Android Composer and DRM/KMS differ only below the shared compositor/raster contract. |

The migration must not create application-side Skia APIs, a Flutter/DOM layer,
an Android-only application binary, or a permanent legacy renderer switch.
It also must not preserve `CommitDisplayList` as the final compositor frame
contract or let a late client stall compositor-owned motion.

## Source anchor

- Branch: `master`
- Commit: `b5db00ae47cd7518b2193ae277bb18680c12fa47`
- Commit subject: `feat: implement cross-process display list serialization and wire protocol for compositor rendering`
- DisplayList protocol: v23 with `UploadImageResource` and
  `CommitDisplayList`
- Android profile: `Google Pixel 8 Pro AVD (Pixel_8_Pro)`
- Physical display: `1344x2992`, 60 Hz
- Logical scale: `4.0`

The visual manifests also record exact compositor, mobile-shell, and UI Demo
artifact hashes. A source commit alone is insufficient evidence when
`--no-build` can deploy older artifacts.

## Automated baseline

Captured on 2026-08-25 after aligning three stale test expectations with
already-established contracts. No production rendering or widget behavior was
changed for this gate.

| Check | Result |
| --- | --- |
| Host build | Passed in the pre-directory-migration capture; rebuild under `out/host` is required |
| Unit/integration suite | Passed in the pre-directory-migration capture; no fixed test count is normative |
| `git diff --check` | Required after every migration change |

The aligned expectations are:

- the checked-in Pixel 8 Pro Gestalt uses scale `4.0`;
- Button scale values come from `lcl-theme`, while interaction motion owns
  timing/easing;
- retained damage receives a one-physical-pixel raster coverage margin before
  clear, clip, and copy.

## Android AVD visual baseline

Captures live under the ignored `out/visual-baselines/` directory. Each scene
contains an immutable `screen.png`, `manifest.json`, raw display state, and the
deployed Gestalt. Capture uses the emulator display below SurfaceFlinger because
LCL owns Composer directly during the session.

| Scene label | Observed baseline |
| --- | --- |
| `step-01-avd-mobile-home-before-skia` | Wallpaper and three application icons render. Application labels are absent. |
| `step-01-avd-terminal-before-skia` | Full-screen dark Terminal surface and gesture pill render. Terminal text is entirely absent. |
| `step-01-avd-ui-demo-before-skia` | Control geometry, fills, borders, clipping, and gesture pill render. Every text label is absent. |

These missing-text results are recorded defects, not approved golden output.
The Skia text step must intentionally change them while preserving unaffected
geometry and color.

## Remaining Step 1 gate

The fixed QEMU/DRM baseline is still pending because no QEMU session was
started for this capture. Before Skia raster implementation begins, record the
same Terminal and UI Demo scenes on QEMU, where text is currently reported to
render. This is necessary to distinguish intentional Android text repair from
cross-platform typography or layout regressions.

Interactive lag is also a known defect, but a still image cannot quantify it.
Motion video and frame timing belong to the later pacing/damage gate; they must
not be inferred from these PNGs.
