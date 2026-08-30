# LCL Core Linux (LCL) Architecture Specification

> **LCL**: LCL Core Linux (Recursive Abbreviation)

LCL is a modular, layered graphics operating system architecture running directly on bare-metal Linux kernel capabilities (`DRM/KMS`, `evdev`, `io_uring`, `AF_UNIX`) without traditional X11 or Wayland display server dependencies. It combines high-performance C++20 engine primitives with native desktop window management and JavaScript/native application runtime capabilities.

---

## 1. Architectural Layers & Vertical Slice

The LCL architecture consists of 5 main decoupled layers:

```text
+-----------------------------------------------------------------------+
|  Top Layer / Apps & Shell (JS Shell, LCL Terminal, System Monitor)     |
+-----------------------------------------------------------------------+
|  Session Layer (lcl-sessiond, app catalog, launch, process lifecycle) |
+-----------------------------------------------------------------------+
|  Application & IPC Layer (lcl-ui clients, compositor AF_UNIX IPC)    |
+-----------------------------------------------------------------------+
|  Layer Production, Window Manager & Presentation Compositor             |
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
* **Logical graphics model (`lcl-graphics`):** Widgets and window chrome emit
  immutable backend-neutral `DisplayList` commands using float logical
  coordinates, paths, paints, clips, layers, text, images, and cached-layer
  references. No device-pixel conversion occurs while the list is recorded.
* **Raster execution (`lcl-rasterd`):** `RasterCanvas` records one frame and
  closes it once at `endFrame()`. The supervised central raster service accepts
  sealed DisplayList/resource memfds through `/Runtime/lcl-raster.sock`, then
  produces an immutable DMA-BUF, AHardwareBuffer, or SHM layer through its
  platform backend. `RenderTarget.deviceScale` is applied only at this boundary.
  Application DisplayList replay never runs in the compositor's vSync-critical
  presentation loop. Desktop uses rotating DMA-BUF layers when render-node
  import is available and retains SHM as its portable fallback. Android uses
  rotating AHardwareBuffer layers as its canonical path and deliberately does
  not hide a broken native transport behind a full-surface CPU copy. The AHB
  handle travels through an ordered private sideband because the public NDK
  exposes only its Unix-socket handle API; `LayerReady` metadata and acquire
  fence remain on rasterd's main private channel. The trusted sideband is
  blocking and the handle is queued first, so `LayerReady` can never expose
  missing storage or a partially published native-handle transaction. Matching
  release fences gate producer slot reuse. GPU-only AHardwareBuffer storage is
  imported as an opaque EGL image; a zero CPU row stride is valid and is not
  interpreted as byte-addressable backing. None of these
  platform transports changes the public application surface protocol.
  Rasterd retains the accepted node revisions, logical cached-layer bodies,
  damage patches, image resources, layer namespaces, and the last immutable
  output base per surface. Cached-layer namespace changes are transactional:
  failed frames do not advance ownership, while removed/replaced nodes release
  their raster cache only after a new immutable layer succeeds.
* **Retained presentation layers:** Small, bounded non-scroll widget subtrees
  that own transform or opacity presentation state are recorded into an
  identity-space rasterd cached layer. Their content revision updates only
  that layer; a pure translation, scale, rotation, or opacity transaction
  reuses its texture and patches the retained composition matrix without a new
  client DisplayList or image upload. Nested candidates collapse into the
  outer useful boundary, candidates containing ScrollView are excluded, and
  identity/removal/geometry changes retire their namespace transactionally.
  Candidates containing an external-buffer node are also excluded so a live
  producer frame is never baked into a stale transform cache.
* **Retained ScrollView tiles:** A ScrollContent cached-layer body is retained
  once in rasterd and split into 384-logical-pixel vertical tiles. Only tiles
  intersecting the viewport plus one tile of overscan remain resident. Pure
  scroll commits carry only the ScrollContent transform; rasterd composes
  resident tiles and lazily creates an entering tile from its retained logical
  template without another client DisplayList or image upload. Damage-scoped
  content patches are retained and applied both to resident tiles and to a tile
  first created later. Distant and removed tiles are explicitly evicted.
* **External-buffer nodes:** Camera, video, and similar producers bind a
  rotating immutable SHM or DMA-BUF frame to a stable retained node. The
  DisplayList contains only that node's placeholder; descriptors and acquire
  fences travel separately over the surface-grant-validated rasterd channel.
  Rasterd samples the frame at the placeholder's exact z-order, publishes only
  its own immutable output layer to compositor, and returns release ownership
  (plus a release fence when available) after the input is no longer sampled.
  The compositor never accepts an application-owned external buffer directly.
* **Font & Text Engine:** Skia owns packaged typeface loading, glyph advances,
  ascent/descent/line-height metrics, UTF-8 drawing, and GPU/CPU rasterization.
  Layout and drawing use the same prepared `SkFont`; no second font rasterizer
  or heuristic measurement path exists.
* **Window Manager:** Decoupled spatial engine tracking z-index, logical
  coordinates (`x, y, width, height`), focus, drag/resize state, and window
  presentation transforms. It paints neither contents nor system UI. Shell-specific
  behavior is selected through `WindowingPolicy`: desktop policy permits
  floating/SSD/CSD move-resize behavior, while mobile policy makes normal app
  surfaces fullscreen and frameless and disables desktop pointer manipulation.
* **Terminal Engine (`TerminalApp` & `PTYManager`):** Pseudo-terminal (`/dev/pts/`) controller spawning interactive GNU Bash shells. Features font-agnostic canvas layout, `TIOCSWINSZ` PTY window size synchronization, VT100 write-head overwrite state tracking, and typing-aware 500ms blinking inverted block cursor rendering.

### III. Session, IPC & Application Bundle Subsystem
* **Location:** `src/core/session/`, `src/core/ipc/`, `src/tools/`
* **Session Authority (`lcl-sessiond`):** Owns the cached `.app` catalog,
  manifest-backed canonical application IDs, default-profile launch, process
  instance IDs, child reaping, and blocking launch completion. It has no
  compositor, surface, scene, renderer, or focus dependency.
* **Session RPC:** `lcl-sessiond` exposes an owner-only `SOCK_SEQPACKET`
  endpoint at `/run/user/1000/lcl-sessiond.sock`. Its explicit little-endian
  requests cover catalog snapshots and launch/process-exit lifecycle; this is
  distinct from compositor protocol v27 surface IPC.
* **Secure Unix Domain Socket IPC:** Compositor protocol v27 operates over Unix Domain `SOCK_SEQPACKET` (`/run/user/1000/lcl-compositor.sock`) with strict `0600` permissions and kernel peer authentication (`SO_PEERCRED`). Application-facing surface IPC carries lifecycle, configure, input, effect, action, producer-grant and frame-feedback messages; it accepts no client layer descriptor or DisplayList commit. A 128-bit surface grant authorizes the same process on rasterd's owner-only socket. Only rasterd's private channels may publish `LayerReady` storage and synchronization handles to the compositor. Edge-to-edge remains platform-neutral, and neither desktop nor mobile policy may rewrite client alpha. `PopupSurface` and `AttachedSurface` are composed and hit-tested inside their parent WindowGroup rather than entering the normal window stack.
* **Native App Bundle Architecture (`.app`):** macOS-style `.app` bundles containing `metadata.json`, `bin/`, and `assets/`. Manifests declare a stable `id`; older bundles receive a deterministic `bundle.<name>` compatibility ID.
* **System Launcher (`lcl-open` / `/usr/bin/open`):** Native C++ session client. It sends `LaunchRequest` to sessiond and optionally waits for `ProcessExited`; it never forks or execs applications itself.

### IV. Storage & Asynchronous File System
* **Location:** `src/fs/`
* **Architecture:** Non-blocking asynchronous I/O over Linux Kernel VFS using `io_uring` and POSIX async primitives to prevent main rendering thread stutters.

### V. Retained Presentation Mission (Normative North Star)

LCL follows a Quartz/Core Animation-style retained presentation boundary. This
is a platform-neutral architectural requirement, not a desktop or mobile
optimization:

1. An application or trusted shell client owns widget layout, DisplayList
   recording, and production of an immutable raster layer. It publishes only a
   complete layer state through an atomic surface commit.
2. The compositor owns a retained presentation tree containing the last
   accepted layer, geometry, transform, opacity, clipping, z-order, effects,
   and presentation timing. Model changes and in-flight presentation values
   remain distinct.
3. Window open/close, move, resize, minimize, restore, overview, and launch
   animations transform or blend already-ready layers. They never wait for
   application layout, font work, DisplayList decoding, or raster replay.
4. If a client misses a frame deadline, the compositor continues presenting
   its last complete layer. A brand-new toplevel with no presentable layer is
   not mapped. A shell-owned launch icon/proxy may animate independently, but it
   must not expose an empty application window.
5. DRM/KMS, Android Composer, and future UEFI substrates consume the same
   committed-layer transaction. Platform differences begin below native-buffer
   import, synchronization, and scanout/present.

Protocol v27 completes this boundary: `CommitDisplayList`, direct SHM/DMA-BUF/
native-buffer attach, and compositor-side application replay are not part of
the surface ABI. The compositor only imports rasterd layers, retains,
transforms, composites, and presents them.

### Strict Atomic WindowGroup Resize

Resize has one non-selectable `AtomicRetained` behavior. Pointer motion updates
only target model geometry and creates a coalescible `geometryGeneration`.
Parent, size-changing attached surfaces, and size-changing popups receive that
generation without parent-first acknowledgement barriers. Until every required
layer and acquire fence is ready, the complete previously presented WindowGroup
keeps its old geometry, transforms, effects, and content. Readiness promotes
the new immutable group snapshot in one display transaction/vSync. There is no
stretch, clip-to-new-geometry, background reveal, snapshot, crossfade, or
timeout-based partial publish. At most one WindowGroup generation is rastered
at a time; newer pointer targets coalesce behind it instead of repeatedly
cancelling in-flight work. Immediately after that complete generation is
presented, the newest queued target becomes the next atomic batch. Pointer
release therefore reaches the final complete generation without raster
starvation or stalling other WindowGroups and compositor-owned animation.
The WindowManager keeps committed presentation geometry separate from the
newest pending model target, so an older in-flight batch can advance the whole
visible WindowGroup without discarding the coalesced target that follows it.

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
|  5. Layer Producer    | (Skia replay -> immutable ready layer commit)
+-----------+-----------+
            |
            v
+-----------+-----------+
|  6. Compositor        | (retained transform/blend -> atomic present)
+-----------------------+
```

---

## 4. Directory Structure

```text
lcl-os/
├── Makefile                        # make build | qemu | qemu NATIVE=1
├── CMakeLists.txt                  # Root CMake build configuration (with BUILD_TESTS support)
├── assets/                         # System fonts and visual assets
│   └── fonts/                      # TrueType font assets (Inter)
├── docs/                           # Documentation & Agent prompts
│   ├── ARCHITECTURE.md             # System architecture specification
│   ├── AG_NEW_INSTANCE_PROMPT.md   # Katrina instance bootstrapper prompt
│   └── Leo_NEW_INSTANCE_PROMPT.md # Leo instance bootstrapper prompt
├── scripts/                        # System build & QEMU launcher
│   ├── run_qemu.py                 # Cross-platform QEMU launcher (Linux/macOS/Windows)
│   ├── run_qemu.sh                 # Thin wrapper -> run_qemu.py
│   ├── Dockerfile.qemu             # linux/amd64 builder image (macOS/Windows)
│   └── fetch_fonts.sh              # Font asset fetcher
├── shell/                          # Shell Presentation Layer
├── tests/                          # CTest & GoogleTest native unit testing suite
│   ├── test_lcl_protocol.cpp       # IPC binary protocol & header tests
│   ├── test_app_bundle_parser.cpp  # .app bundle metadata parsing & directory scan
│   └── test_ipc_manager.cpp        # Unix Domain Socket & SO_PEERCRED authentication
└── src/                            # Core C++20 Engine & Applications
    ├── main.cpp                    # Application entry point & compositor loop
    ├── apps/                       # Native system applications
    │   ├── sysmon/                 # System Monitor application
    │   └── terminal/               # LCL Terminal application & VT100 engine
    ├── core/                       # Core engine subsystems
    │   ├── display/                # DRM/KMS & OpenGL/Vulkan display backend
    │   ├── input/                  # evdev & libinput event listeners
    │   ├── ipc/                    # Secure Unix Domain Socket IPC server
    │   ├── session/                # App registry, session RPC, lifecycle authority
    │   └── terminal/               # PTY master/slave manager
    ├── render/                     # Layer-production raster and presentation primitives
    ├── fs/                         # io_uring & POSIX async file system
    └── tools/                      # Native CLI utilities (lcl-open, lcl-sessiond)
```

### Build & QEMU (dev hosts)

**Host-side Fast Unit Testing** (Runs in ~0.03 seconds natively on Linux host without QEMU):
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --target lcl_unit_tests
ctest --test-dir build --output-on-failure
```

**System Integration & Compositor Execution (QEMU Docker Builder)**:
```bash
make qemu            # default 1280x800, host refresh rate
make qemu NATIVE=1   # host resolution + scale + fullscreen
```

Guest display configuration (when `NATIVE=1`):
- The standard kernel `video=` argument establishes the early physical DRM mode.
- LCL resolution, refresh and scale policy comes from the versioned Gestalt JSON;
  QEMU supplies it through `fw_cfg`, not LCL-specific kernel arguments.
- QEMU cocoa: `full-screen=on,zoom-to-fit=on` (fill screen if mode list is inexact)

Gestalt lookup is platform-owned. Android ports install their profile at
`/vendor/etc/lcl/gestalt.json`; desktop/rootfs images use
`/System/Library/Gestalt/default.json`. Development launches can override either
path with the `--gestalt /absolute/profile.json` option (or the equivalent
`LCL_GESTALT_PATH` environment variable). A requested override is strict and
startup fails if it is missing or invalid. A missing platform-default file is
not fatal and selects the built-in rectangular 1x profile.

The version 1 root accepts an optional `shell` value (`desktop` or `mobile`),
defaulting to `desktop` for older profiles. Both executables are installed in
the canonical rootfs; `lcl-shell-launcher` reads the same Gestalt document as
the platform and replaces itself with exactly one selected shell. Emulator
viewer flags do not form a second shell-selection authority. The compositor
uses that same value to select `DesktopWindowPolicy` or `MobileWindowPolicy`;
the shell executable and its window-management model therefore cannot drift.

The version 1 `display` object accepts the optional `width`, `height`,
`refreshRateHz`, and `scale` mode fields, plus `naturalOrientation`,
`defaultRotation`, `safeArea`, per-corner geometry in `corners`, and rectangular
`cutouts`. Unknown fields are rejected so a misspelled ROM profile cannot be
silently accepted. The canonical desktop source profile is
`config/gestalt/default.json`; the QEMU mobile source profile is
`config/gestalt/mobile.json`. The launcher derives an instance-local runtime
copy from the selected source and applies only the current output mode and
explicit command-line overrides to that copy.

The active `IDisplayBackend` prefers the Gestalt mode when the platform exposes
that mode, and stores the Gestalt scale in its `DisplayMode.scaleFactor`. If
Gestalt omits resolution, Android Composer or desktop DRM supplies the active
mode; if it omits scale, the backend uses `1.0`. Compositor output
initialization transfers that value into `RenderTarget.deviceScale`; widgets,
chrome, paths, strokes, text, images, and effects remain in logical units and
receive that scale only
during raster replay. `WindowGroupTransform` owns the forward and inverse
presentation matrices shared by client content, chrome, popups, effects, and
hit testing. Physical input is converted to output-logical coordinates once;
window-local input then uses the inverse group transform.

---

## 5. Window Management & Compositing Layer Separation

`lcl-os` grafik ve pencere katmanında sorumlulukların ayrıştırılması (Separation of Concerns) kesin kurallarla tanımlanmıştır:

1. **Client Applications (User Space / UI Kits):** Kendi iç düzenini (Flexbox, Grid, Monospace Cell) ve mantıksal `DisplayList` kaydını yönetir. Producer-side raster aşaması bu kaydı compositor presentation döngüsünün dışında hazır, immutable bir layer'a dönüştürür. WM'den gelen `ConfigureBounds` isteklerini mantıksal ölçülerle alır; rasterd platform backend'i uygun fiziksel DMA-BUF veya AHardwareBuffer backing'ini oluşturur. Desktop native import bulunmadığında aynı sözleşmenin `memfd`/SHM fallback'ini kullanabilir; Android AHardwareBuffer yolunu zorunlu tutar. `bufferScale` yalnız buffer tahsisi ve eşleme sınırındadır. İstemci WM veya Compositor'ün ekran koordinatları (\(X, Y\)) hakkında bilgi sahibi değildir.
2. **Window Manager (WM):** Pencere geometrisi, odak yönetimi, sürükleme/boyutlandırma durum makinelerinin (`WM Drag/Resize State`) tek sahibidir. Sürüklenen kenara (`ResizeEdge`) göre sabit kalacak anchor noktasını korur. Client'tan gelen gerçek tampon boyutunu (\(frameW, frameH\)) kabul eder, `commitSurfaceGeometry` metodu üzerinden offset hesabını yapar ve pencerenin nihai dünya koordinatlarını (\(X_{final}, Y_{final}\)) belirler. Uygulamaya özel kod barındıramaz.
3. **Compositor (Presentation Engine):** Hazır layer'ların import/retention işlemini, z-index harmanlamasını, presentation tree transform/effect'lerini ve vSync eşzamanlamasını üstlenir. Pencere durum makinelerinin veya client çiziminin sahibi değildir; WM'in onayladığı mantıksal geometriyi, ortak `WindowGroupTransform`u ve Client'ın atomik olarak commit ettiği son hazır layer'ı birleştirir. Compositor `lcl-ui`ya veya `lcl-window-chrome`a bağlanmaz ve presentation deadline içinde application DisplayList replay etmez.

Window-local transient UI ayrı bir surface veya ikinci bir layout ağacı değildir. `WindowApp` içindeki widget-olmayan `TransientController` yalnız stable handle, dismissal ve teardown policy'sini yönetir; local transient primitive gerektiğinde tek normal WindowRoot ağacında absolute Widget sibling olarak çizilir. `Popover v1` ise her zaman parent-bound `PopupSurface v1` kullanır.

`Popover v1`, generic content'i hosted `WindowApp` üzerinden anchor-altı ve left-aligned bir `PopupSurface` içinde sunar; pencere içine sığma durumuna göre ikinci bir presentation yolu seçmez. Generic content sahipliği yalnız lcl-ui Popover katmanındadır; compositor Popover kavramını bilmez.

### Compositor içi sorumluluklar

`Compositor`, alt sistemleri başlatır ve ana döngüyü sıralar; client kaynakları veya
çizim ayrıntıları için ikinci bir sahip değildir.

- `SurfaceRegistry`: `(client, surfaceId)` kimliği, producer grant'i, son hazır
  raster layer, pending generation ve presentation credit'inin tek sahibidir.
  Disconnect, kapatma geçişi ve shutdown aynı idempotent kaynak serbest bırakma
  yolunu kullanır; application DisplayList/image pixel kaynağı tutmaz.
- `ProtocolDispatcher`: App-facing lifecycle/action/effect mesajlarını ve private
  rasterd `LayerReady` kabulünü doğrular; başka kanaldan layer kabul etmez.
- `InputRouter`: WindowManager'ın hit-test/focus/resize sonucunu client'a logical
  koordinatlı input ve configure mesajlarına dönüştürür.
- `FrameScheduler`: frame bütçesi, cursor blink ve surface enter/close
  transition zamanlamasını yönetir.
- `CompositorRenderer`: o frame için salt-okunur retained presentation snapshot'ını
  compose ve present eder. Her surface için son tam layer'ı kullanır; yeni
  client içeriği hazır değilse eski layer'ı korur. Snapshot yalnızca frame
  boyunca geçerli `const` görünüm taşır ve client raster işini tetiklemez.

---

## 6. Blind Composition & WM-Owned Decoration

1. **Sub-surface Grouping & Atomic Z-Stacking (`window_manager` & `compositor`):**
   - Her pencere ve ona ait tüm generic attached surface'ler tek bir **Pencere Grubu (Window Stack Element)** olarak ele alınır.
   - Compositor render döngüsü (`renderFrame`), `WindowManager` z-order sıralamasını (`m_windows` vektörünü) en alttan en üste doğru izler.
   - Compositor çocukların UI anlamını bilmeden app buffer ve attached surface'leri generic rol/sıra ile compose eder. Odaklanan pencere atomik olarak Z-Stack'in en üstüne (\(Z_{\text{max}}\)) yükseltildiğinde bütün grup birlikte taşınır.

2. **Generic Attached Surface Protocol (`lcl_protocol`):**
   - Güvenilir presentation client'ları `AttachedSurfaceCreate` ile başka bir
     toplevel'in WindowGroup'una `Frame` veya `Adornment` yüzeyi ekler.
   - DesktopWM `lcl-window-chrome` ile titlebar'ın layout, hit-test, action,
     motion ve `DisplayList` üretimini yapar; bunu hazır attached layer'a
     rasterize edip atomik commit eder ve `RequestManagedWindowAction` ile hedef
     toplevel'e drag/minimize/maximize/close isteği yollar.
   - MobileWM gesture indicator'ı `Adornment` olarak kaydeder. Compositor
     titlebar, button veya gesture-pill türlerini ve renk/theme tokenlarını
     içermez; yalnız buffer yaşam döngüsü, transform, opacity, z-order, input
     capture ve yetki doğrulamasını uygular.

---

## 7. Unix Domain Socket IPC & Disconnect Detection

1. **Secure Domain Socket Protocol:** Compositor IPC v27 operates on `/run/user/1000/lcl-compositor.sock` as `SOCK_SEQPACKET`, with `0600` permissions and kernel peer authentication (`SO_PEERCRED`). Rasterd uses a separate owner-only public producer socket plus a private compositor socketpair.
2. **Orderly Socket EOF Handling:** When a client process exits or terminates (`Ctrl+C`), `recvmsg()` returns `0` (EOF). The v27 transport reports this as `ReceiveStatus::Closed`, independently of stale `errno` values.
3. **Decoupled Surface & Window Reclamation:** `IPCManager` emits a typed disconnect event upon socket EOF. `ProtocolDispatcher` ilgili pencereyi `WindowManager`dan kaldırır; `SurfaceRegistry` SHM eşlemesini ve memfd'yi tek sahip olarak serbest bırakır.

---

## 8. Multi-Platform Substrates & Stage 4C.3 Architecture

LCL OS decouples the user-space runtime environment from the underlying platform substrate:

```text
+-----------------------------------------------------------------------------------------+
|                  Canonical LCL Userspace (build/rootfs/lcl-rootfs-x86_64.ext4)          |
|  - Desktop Apps: Terminal.app, UIDemo.app, UIDemoJS.app, ShaderDemo.app                |
|  - System Daemons: lcl-sessiond, lcl-desktop-shell, lcl-open                           |
|  - User Home (/Users/Rei), Shell (/System/Tools/bash), Fonts, C/C++ glibc Libraries     |
+-----------------------------------------------------------------------------------------+
                                         │
                 ┌───────────────────────┴───────────────────────┐
                 │  Shared IPC Bridge: /Runtime/*.sock           │
                 ▼                                               ▼
+─────────────────────────────────+             +─────────────────────────────────+
|   Bare-Metal Linux Substrate    |             |    Android Substrate (Stage 4C) |
|   - Driver: Linux DRM/KMS       |             | - Driver: Composer3 AIDL / 2.2-2.4 HIDL|
|   - Compositor: lcl-core        |             |    - Compositor: lcl-core-android|
|   - Input: evdev / libinput     |             |    - Mount: /mnt/lcl ext4        |
|   - Init: Limine / custom init  |             |    - Init: Android init / lcl.rc |
+─────────────────────────────────+             +─────────────────────────────────+
```

### Key Invariants of Stage 4C.3
1. **Same-Binary Invariant:** All user-facing binaries and system daemons are built once and packaged into `lcl-rootfs-x86_64.ext4`. They run unmodified across bare-metal Linux and Android AVD.
2. **Minimal Substrate Footprint:** Android `system.img` contains ONLY platform-specific substrate components (`lcl-core-android`, `lcl.rc`, `lcl-bootstrap.sh`). Zero user applications or desktop daemons reside inside `system.img`.
3. **Idempotent RootFS Attachment & Probe Isolation:** Canonical rootfs is attached as an independent raw block device (`-drive file=...,format=raw`) and mounted on `/mnt/lcl`. Probing for block device discovery is performed on `/mnt/lcl-probe` and immediately unmounted to prevent overlay stacking and `EBUSY` remount errors.
4. **Kernel devpts Bind-Mount:** The kernel `devpts` pseudo-filesystem is explicitly bind-mounted (`/dev/pts` -> `/mnt/lcl/dev/pts`) to ensure glibc `posix_openpt()` can allocate pseudo-terminals for `Terminal.app` without `ENODEV`.
5. **Shared `/Runtime` tmpfs IPC Bridge:** `/Runtime` tmpfs is bind-mounted into `/mnt/lcl/Runtime`, enabling transparent little-endian `SOCK_SEQPACKET` IPC communication between `lcl-core-android` (host substrate) and canonical desktop processes.
