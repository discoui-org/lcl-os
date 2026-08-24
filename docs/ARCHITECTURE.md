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
|  Graphics & Window Manager (`lcl-graphics`, `lcl-raster`, FontRenderer)     |
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
* **Raster execution (`lcl-raster`):** `RasterCanvas` records one frame and
  submits it once at `endFrame()`. `RasterRenderer` replays that list through
  the GLES or software backend. `RenderTarget.deviceScale` is applied only at
  this raster boundary.
* **Font & Text Engine (`FontRenderer`):** TrueType vector font rasterization via `stb_truetype` featuring subpixel antialiasing, macOS-style gamma correction, font-agnostic metric queries (`getCellWidth()`, `getCellHeight()`), and UTF-8 multi-byte sequence handling.
* **Window Manager:** Decoupled spatial engine tracking z-index, logical
  coordinates (`x, y, width, height`), focus, drag/resize state, and window
  presentation transforms. It does not paint window contents.
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
  distinct from compositor protocol v15 surface IPC.
* **Secure Unix Domain Socket IPC:** Compositor protocol v15 operates over Unix Domain `SOCK_SEQPACKET` (`/run/user/1000/lcl-compositor.sock`) with strict `0600` permissions. Explicit little-endian packets preserve payload and `SCM_RIGHTS` boundaries; kernel peer authentication (`SO_PEERCRED`) supplies `PID`, `UID`, and `GID`. Live resize carries independent logical content/backing extents, SHM damage, compositor presentation timestamps, and explicit discarded-frame credits. Edge-to-edge is a platform-neutral surface policy: one outer-surface effect chain can extend beneath desktop or mobile system insets while client widgets remain inside the safe content rect. `PopupSurface v1` reuses the same buffer infrastructure while binding a popup to a same-process parent surface; it is composed and hit-tested inside that parent's WindowGroup rather than entering the normal window stack.
* **Native App Bundle Architecture (`.app`):** macOS-style `.app` bundles containing `metadata.json`, `bin/`, and `assets/`. Manifests declare a stable `id`; older bundles receive a deterministic `bundle.<name>` compatibility ID.
* **System Launcher (`lcl-open` / `/usr/bin/open`):** Native C++ session client. It sends `LaunchRequest` to sessiond and optionally waits for `ProcessExited`; it never forks or execs applications itself.

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
    ├── render/                     # Renderer engine & stb_truetype FontRenderer
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

The version 1 `display` object accepts the optional `width`, `height`,
`refreshRateHz`, and `scale` mode fields, plus `naturalOrientation`,
`defaultRotation`, `safeArea`, per-corner geometry in `corners`, and rectangular
`cutouts`. Unknown fields are rejected so a misspelled ROM profile cannot be
silently accepted. The canonical desktop example is
`config/gestalt/default.json`.

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

1. **Client Applications (User Space / UI Kits):** Kendi iç düzenini (Flexbox, Grid, Monospace Cell) yönetir. WM'den gelen `ConfigureBounds` isteklerini mantıksal ölçülerle alır. Uygun fiziksel DMA-BUF backing'i oluşturur; desteklenmeyen ortamlarda aynı sözleşmenin `memfd`/SHM fallback'ini kullanır. `bufferScale` yalnız buffer tahsisi ve eşleme sınırındadır. İstemci WM veya Compositor'ün ekran koordinatları (\(X, Y\)) hakkında bilgi sahibi değildir.
2. **Window Manager (WM):** Pencere geometrisi, odak yönetimi, sürükleme/boyutlandırma durum makinelerinin (`WM Drag/Resize State`) tek sahibidir. Sürüklenen kenara (`ResizeEdge`) göre sabit kalacak anchor noktasını korur. Client'tan gelen gerçek tampon boyutunu (\(frameW, frameH\)) kabul eder, `commitSurfaceGeometry` metodu üzerinden offset hesabını yapar ve pencerenin nihai dünya koordinatlarını (\(X_{final}, Y_{final}\)) belirler. Uygulamaya özel kod barındıramaz.
3. **Compositor (Presentation Engine):** Tamponların ekrana çizimi, z-index harmanlaması (blending), sistem chrome display list'lerinin replay'i ve vSync eşzamanlamasını üstlenir. Pencere durum makinelerinin sahibi değildir; WM'in onayladığı mantıksal geometriyi, ortak `WindowGroupTransform`u ve Client'ın sunduğu fiziksel tamponu vSync anında atomik olarak birleştirir. Compositor `lcl-ui`ya bağlanmaz; `lcl-graphics`, `lcl-raster` ve `lcl-window-chrome` sınırında kalır.

Window-local transient UI ayrı bir surface veya ikinci bir layout ağacı değildir. `WindowApp` içindeki widget-olmayan `TransientController` yalnız stable handle, dismissal ve teardown policy'sini yönetir; local transient primitive gerektiğinde tek normal WindowRoot ağacında absolute Widget sibling olarak çizilir. `Popover v1` ise her zaman parent-bound `PopupSurface v1` kullanır.

`Popover v1`, generic content'i hosted `WindowApp` üzerinden anchor-altı ve left-aligned bir `PopupSurface` içinde sunar; pencere içine sığma durumuna göre ikinci bir presentation yolu seçmez. Generic content sahipliği yalnız lcl-ui Popover katmanındadır; compositor Popover kavramını bilmez.

### Compositor içi sorumluluklar

`Compositor`, alt sistemleri başlatır ve ana döngüyü sıralar; client kaynakları veya
çizim ayrıntıları için ikinci bir sahip değildir.

- `SurfaceRegistry`: `(client, surfaceId)` yüzey kaydı ile memfd/SHM eşlemesinin
  tek sahibidir. Disconnect, kapatma geçişi ve shutdown aynı idempotent kaynak
  serbest bırakma yolunu kullanır.
- `ProtocolDispatcher`: IPC mesajlarını doğrular/dispatch eder; client rolü,
  surface create/attach, effect graph ve shell window-list yayınını yönetir.
- `InputRouter`: WindowManager'ın hit-test/focus/resize sonucunu client'a logical
  koordinatlı input ve configure mesajlarına dönüştürür.
- `FrameScheduler`: frame bütçesi, cursor blink ve surface enter/close
  transition zamanlamasını yönetir.
- `CompositorRenderer`: o frame için salt-okunur `SurfaceRegistry` snapshot'ını
  çizer ve present eder. Snapshot entry'leri SHM pixel veya effect graph kopyası
  içermez; yalnızca frame boyunca geçerli `const` görünüm taşır.

---

## 6. Z-Indexing & Hybrid Decoration Protocol (SSD/CSD Negotiation)

1. **Sub-surface Grouping & Atomic Z-Stacking (`window_manager` & `compositor`):**
   - Her pencere ve ona ait tüm alt yüzeyler (Titlebar + Window Border + Client SHM Buffer) tek bir **Pencere Grubu (Window Stack Element)** olarak ele alınır.
   - Compositor render döngüsü (`renderFrame`), `WindowManager` z-order sıralamasını (`m_windows` vektörünü) en alttan en üste doğru izler.
   - Her pencere için önce başlık çubuğu/çerçeve (SSD), hemen ardından istemcinin SHM tamponu çizilir. Odaklanan pencere atomik olarak Z-Stack'in en üstüne (\(Z_{\text{max}}\)) yükseltildiğinde hem çerçeve hem de içerik tamponu en üste taşınır.

2. **Common Decoration Negotiation Protocol (`lcl_protocol`):**
   - `LCLOpcode::SetDecorationMode` IPC mesajı ile istemciler `SSD` (Server-Side Decoration) veya `CSD` (Client-Side Decoration) modlarını talep eder.
   - **Compositor WindowManager:** Varsayılan olarak `SSD` modunda pencere başlık çubuğunu çizer. İstemci `CSD` talep ederse başlık çubuğu çizimini devre dışı bırakarak tüm render alanını istemciye devreder.
   - Her iki mod da renderer bağımsız `lcl-window-chrome` hedefindeki aynı
     `WindowChromeWidget` layout, hit-test, aksiyon, interaction, motion ve
     görsel renk çözümlemesini kullanır. Aynı stil girdileri aynı mantıksal
     `DisplayList`i üretir: CSD bu listeyi client `Canvas`ına kaydeder, SSD ise
     compositor raster hedefinde replay eder. İkinci bir chrome painter veya
     kontrol ağacı yoktur.
   - Mobil shell gelecekte aynı compositor ve revisioned shell-state sözleşmesini tüketir; ayrı bir `lcl-mobile-wm` veya paralel pencere otoritesi yoktur.

---

## 7. Unix Domain Socket IPC & Disconnect Detection

1. **Secure Domain Socket Protocol:** Compositor IPC v15 operates on `/run/user/1000/lcl-compositor.sock` as `SOCK_SEQPACKET`, with `0600` permissions and kernel peer authentication (`SO_PEERCRED`).
2. **Orderly Socket EOF Handling:** When a client process exits or terminates (`Ctrl+C`), `recvmsg()` returns `0` (EOF). The v15 transport reports this as `ReceiveStatus::Closed`, independently of stale `errno` values.
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
