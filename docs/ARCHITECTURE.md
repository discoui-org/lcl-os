# LCL Core Linux (LCL) Architecture Specification

> **LCL**: LCL Core Linux (Recursive Abbreviation)

LCL is a modular, layered graphics operating system architecture running directly on bare-metal Linux kernel capabilities (`DRM/KMS`, `evdev`, `io_uring`, `AF_UNIX`) without traditional X11 or Wayland display server dependencies. It combines high-performance C++20 engine primitives with native desktop window management and JavaScript/native application runtime capabilities.

---

## 1. Architectural Layers & Vertical Slice

The LCL architecture consists of 4 main decoupled layers:

```text
+-----------------------------------------------------------------------+
|  Top Layer / Apps & Shell (JS Shell, LCL Terminal, System Monitor)     |
+-----------------------------------------------------------------------+
|  Application & IPC Layer (AppBundleParser, lcl-open, AF_UNIX IPC)     |
+-----------------------------------------------------------------------+
|  Graphics & Window Manager (Skia/stb_truetype, FontRenderer, PTY)     |
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
* **Font & Text Engine (`FontRenderer`):** TrueType vector font rasterization via `stb_truetype` featuring subpixel antialiasing, macOS-style gamma correction, font-agnostic metric queries (`getCellWidth()`, `getCellHeight()`), and UTF-8 multi-byte sequence handling.
* **Window Manager:** Decoupled spatial window layout engine tracking z-index, spatial coordinates (`x, y, width, height`), focus, and window frame rendering.
* **Terminal Engine (`TerminalApp` & `PTYManager`):** Pseudo-terminal (`/dev/pts/`) controller spawning interactive GNU Bash shells. Features font-agnostic canvas layout, `TIOCSWINSZ` PTY window size synchronization, VT100 write-head overwrite state tracking, and typing-aware 500ms blinking inverted block cursor rendering.

### III. IPC & Application Bundle Subsystem
* **Location:** `src/core/ipc/`, `src/tools/`
* **Secure Unix Domain Socket IPC:** Compositor protocol v3 operates over Unix Domain `SOCK_SEQPACKET` (`/run/user/1000/lcl-compositor.sock`) with strict `0600` permissions. Explicit little-endian packets preserve payload and `SCM_RIGHTS` boundaries; kernel peer authentication (`SO_PEERCRED`) supplies `PID`, `UID`, and `GID`.
* **Native App Bundle Architecture (`.app`):** macOS-style `.app` bundles containing `metadata.json`, `bin/`, and `assets/`.
* **System Launcher (`lcl-open` / `/usr/bin/open`):** Native C++ CLI tool linking against `AppBundleParser`. Supports background launch (`open App.app`) and blocking wait mode (`open -w App.app`) with signal handling (`SIGINT`/`SIGTERM`) and automatic window surface reclamation (`DESTROY_LAST_WINDOW`).

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
│   ├── test_display_scale.cpp      # HiDPI scale factor & pixel conversion math
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
    │   └── terminal/               # PTY master/slave manager
    ├── render/                     # Renderer engine & stb_truetype FontRenderer
    ├── fs/                         # io_uring & POSIX async file system
    └── tools/                      # Native CLI utilities (lcl-open)
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

Guest display boot args (when `NATIVE=1`):
- HiDPI/Retina: `video=` = **physical** pixels, `lcl.scale` = DPR (sharp UI; avoids zoom-upscale blur)
- 1x displays: `video=` = host resolution, `lcl.scale=1`
- QEMU cocoa: `full-screen=on,zoom-to-fit=on` (fill screen if mode list is inexact)

`DisplayManager` prefers boot-requested mode. `DisplayScale` multiplies fonts/chrome/windows by `lcl.scale`.

---

## 5. Window Management & Compositing Layer Separation

`lcl-os` grafik ve pencere katmanında sorumlulukların ayrıştırılması (Separation of Concerns) kesin kurallarla tanımlanmıştır:

1. **Client Applications (User Space / UI Kits):** Kendi iç düzenini (Flexbox, Grid, Monospace Cell) yönetir. WM'den gelen pencere resize isteklerini (`RESIZE_REQUEST` / `ConfigureBounds`) alır. Kendi mantığına göre uygun boyutta SHM buffer allocate eder ve `ATTACH_BUFFER` ile sunar. WM veya Compositor'ün ekran koordinatları (\(X, Y\)) hakkında bilgi sahibi değildir.
2. **Window Manager (WM):** Pencere geometrisi, odak yönetimi, sürükleme/boyutlandırma durum makinelerinin (`WM Drag/Resize State`) tek sahibidir. Sürüklenen kenara (`ResizeEdge`) göre sabit kalacak anchor noktasını korur. Client'tan gelen gerçek tampon boyutunu (\(frameW, frameH\)) kabul eder, `commitSurfaceGeometry` metodu üzerinden offset hesabını yapar ve pencerenin nihai dünya koordinatlarını (\(X_{final}, Y_{final}\)) belirler. Uygulamaya özel kod barındıramaz.
3. **Compositor (Presentation Engine):** "Kör Çizici" (Blind Renderer) olarak çalışır. Tamponların ekrana çizimi, z-index harmanlaması (blending) ve vSync eşzamanlamasını üstlenir. Pencere durum makinelerinden veya kenar hesaplarından bağımsızdır. WM'in onayladığı geometriyi ve Client'ın sunduğu tamponu vSync anında atomic olarak ekrana çeker.

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
   - **Masaüstü WM (`lcl-desktop-wm`):** Varsayılan olarak `SSD` modunda pencere başlık çubuğunu çizer. İstemci `CSD` talep ederse başlık çubuğu çizimini devre dışı bırakarak tüm render alanını istemciye devreder.
   - **Mobil WM (`lcl-mobile-wm`):** Mobil deneyiminde `SSD` çizimini reddederek uygulamalara tam ekran (fullscreen) uygulama alanı sağlar.

---

## 7. Unix Domain Socket IPC & Disconnect Detection

1. **Secure Domain Socket Protocol:** Compositor IPC v3 operates on `/run/user/1000/lcl-compositor.sock` as `SOCK_SEQPACKET`, with `0600` permissions and kernel peer authentication (`SO_PEERCRED`).
2. **Orderly Socket EOF Handling:** When a client process exits or terminates (`Ctrl+C`), `recvmsg()` returns `0` (EOF). The v3 transport reports this as `ReceiveStatus::Closed`, independently of stale `errno` values.
3. **Decoupled Surface & Window Reclamation:** `IPCManager` emits a typed disconnect event upon socket EOF. `ProtocolDispatcher` ilgili pencereyi `WindowManager`dan kaldırır; `SurfaceRegistry` SHM eşlemesini ve memfd'yi tek sahip olarak serbest bırakır.
