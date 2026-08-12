# LCL Core Linux (LCL OS)

Modular, Lightweight Linux Distribution built with C++20 and Limine bootloader. This is a custom Linux distribution built from scratch with a focus on modularity, performance, and ease of use. I made this to learn about operating systems and to create a lightweight desktop environment that is easy to use and customize. This is a work in progress and is not intended for production use but I have plans to continue developing it for my personal use.

## Project Structure

```text
lcl-os/
├── iso_root/                 # Limine bootloader configuration & boot tree
├── src/                      # Core OS Engine & Compositor
│   ├── core/                 # DRM/KMS, EGL, Evdev Input, Hotplug, IPC, Session
│   ├── render/               # Skia Renderer, FontRenderer, WindowManager
│   └── tools/                # Core System Daemons & Binaries (lcl-core, lcl-terminal, lcl-open)
├── lcl-ui/                   # Decoupled UI Application Framework (Yoga Flexbox, Widget Tree)
├── apps/                     # User-Space Desktop Applications
│   ├── ui_demo/              # Flexbox Interactive Widget Demo App (UIDemo.app)
│   └── shader_demo/          # 144Hz Procedural Shader & Animation App (ShaderDemo.app)
├── docs/                     # Technical Documentation & Guides
│   ├── ARCHITECTURE.md       # Low-level system & architectural design
│   ├── LCL_UI_FRAMEWORK.md   # Complete lcl-ui developer API guide
│   └── DEVELOPER_GUIDE.md    # Build, testing, and contribution instructions
└── scripts/                  # QEMU Isolated Boot Launcher, ISO Builder & Packaging
```

---

## Quick Start & Building

### Prerequisites
- Linux host or Docker (the build system automatically uses an isolated Docker container `lcl-os-qemu-builder` if local toolchain is absent).
- CMake 3.20+ and C++20 compiler (`g++-13` or `clang-16`+).
- `xorriso` and `limine` (for generating bootable ISO images).
- QEMU (`qemu-system-x86_64`) for running the test environment.

### Building
Compile all core binaries, libraries, unit tests, and application bundles:
```bash
make
```

### Building Bootable ISO Image (`build/lcl-os.iso`)
Generate a hybrid bootable LiveUSB / ISO image for bare-metal hardware or VMs:
```bash
make iso
```

### Running in QEMU Virtual Machine

#### 1. Fast Direct Kernel Boot (Development)
```bash
make qemu GPU=1 NATIVE=1
```

#### 2. Boot Limine ISO in Legacy BIOS Mode
```bash
make qemu-iso
```

#### 3. Boot Limine ISO in UEFI Mode (OVMF Firmware)
```bash
make qemu-iso UEFI=1
```

---

## Running Applications Inside LCL OS

When LCL OS boots in QEMU, launch applications from the built-in terminal or using the `open` command:

- **Launch 144Hz Procedural Shader Demo:**
  ```bash
  lcl_shader_demo
  # or
  open ShaderDemo.app
  ```
- **Launch Interactive Flexbox UI Demo:**
  ```bash
  lcl_ui_demo
  # or
  open UIDemo.app
  ```
- **Launch Additional Terminal Instances:**
  ```bash
  open Terminal.app
  ```

--

## Running Unit Tests

Run the full GoogleTest CTest suite (22 passing unit tests covering IPC protocol, input hotplug/touchpad math, bundle parsing, display scaling, Yoga layout, and event routing):
```bash
make test
```

---

## Documentation

- [Architecture Specification](docs/ARCHITECTURE.md)
- [lcl-ui Framework Developer Guide](docs/LCL_UI_FRAMEWORK.md)
- [Developer & Testing Guide](docs/DEVELOPER_GUIDE.md)

---

## License

MIT License
