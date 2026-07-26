#include "core/compositor/compositor.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sys/mman.h>

namespace lcl::core {

// ============================================================
// Construction / Destruction
// ============================================================

Compositor::Compositor() = default;

Compositor::~Compositor() {
    if (!m_initialized) return;

    // 1. IPC — close socket before renderer/input tear-down
    m_ipcManager.shutdown();

    // 2. Renderer — release DRM framebuffer
    m_renderer.shutdown();

    // 3. Input — close evdev/libinput handles
    m_inputManager.shutdown();

    // 4. Display — release DRM/KMS lease
    m_displayManager.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: "
              << m_loopTicks << "\n";
}

// ============================================================
// Initialization
// ============================================================

bool Compositor::initialize() {
    if (m_initialized) return true;

    std::cout << "====================================================\n"
              << "  LCL Core Linux (LCL) v0.1.0 - Core Engine\n"
              << "  Architecture: Direct DRM/KMS & evdev (No X11/Wayland)\n"
              << "  C++ Standard: C++20\n"
              << "====================================================\n"
              << "[LCL Core] Initializing pure Display Server compositor...\n";

    // --- Display scale (reads lcl.scale= from /proc/cmdline) ---
    DisplayScale::initialize();

    // --- DRM/KMS Display ---
    if (!m_displayManager.initialize("/dev/dri/card0")) {
        std::cout << "[LCL Core] Display subsystem running in fallback/skeleton mode.\n";
    }

    // --- Input (libinput → evdev fallback) ---
    if (!m_inputManager.initialize("seat0")) {
        std::cout << "[LCL Core] Input subsystem running in fallback/skeleton mode.\n";
    }

    // --- Renderer ---
    if (!m_renderer.initialize(&m_displayManager)) {
        std::cout << "[LCL Core] Renderer running in fallback mode.\n";
    }

    // --- Window Manager Canvas ---
    m_windowManager.initialize(m_renderer.getWidth(), m_renderer.getHeight());

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(kCompositorSocket);

    // --- Wire input events → WindowManager ---
    m_inputManager.setEventCallback([this](const InputEvent& ev) {
        if (m_windowManager.processInputEvent(ev)) {
            m_needsRedraw = true;
        }
    });

    // --- Launch primary desktop session ---
    m_startupManager.launchDefaultSession(m_windowManager);

    // --- Frame pacing from DRM refresh rate ---
    uint32_t hz = (m_displayManager.isInitialized()
                   ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (hz == 0) hz = 60;
    m_targetFrameDuration = std::chrono::microseconds(1000000 / hz);

    std::cout << "[LCL Core] Pure Display Server active on " << kCompositorSocket
              << ". Listening for client surface registrations.\n"
              << "[LCL Core] High Refresh Rate active: targeting " << hz
              << " Hz (~" << (1000000 / hz) << " us per frame budget).\n";

    m_lastBlinkCheck = std::chrono::steady_clock::now();
    m_initialized = true;
    return true;
}

// ============================================================
// Main Event Loop
// ============================================================

void Compositor::run() {
    if (!m_initialized) return;

    while (m_running.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        processInput();
        processIPC();
        tickCursorBlink();
        renderFrame();

        // Dynamic frame pacing
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - frameStart);
        if (elapsed < m_targetFrameDuration) {
            std::this_thread::sleep_for(m_targetFrameDuration - elapsed);
        }
        ++m_loopTicks;
    }
}

// ============================================================
// Per-Tick Processing
// ============================================================

void Compositor::processInput() {
    if (m_inputManager.isInitialized()) {
        m_inputManager.dispatchEvents(
            static_cast<int>(m_renderer.getWidth()),
            static_cast<int>(m_renderer.getHeight()));
    }
}

void Compositor::processIPC() {
    for (const auto& msg : m_ipcManager.pollMessages()) {
        // --- SURFACE_CREATE: register a window on the compositor canvas ---
        bool isSurfaceCreate = (msg.header.opcode == lcl::protocol::LCLOpcode::SurfaceCreate) ||
                               (msg.command.rfind("SURFACE_CREATE:", 0) == 0);
        bool isAttachBuffer  = (msg.header.opcode == lcl::protocol::LCLOpcode::AttachBuffer) ||
                               (msg.command.rfind("ATTACH_BUFFER:", 0) == 0);

        if (isSurfaceCreate) {
            uint32_t surfId = 1;
            std::string title = "LCL Terminal";
            int winX = DisplayScale::px(80);
            int winY = DisplayScale::px(60);
            int winW = DisplayScale::px(540);
            int winH = DisplayScale::px(360);

            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSurfaceCreate)) {
                auto* sm = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data());
                surfId = sm->surfaceId;
                if (sm->title[0]) title = sm->title;
                if (sm->x > 0) winX = sm->x;
                if (sm->y > 0) winY = sm->y;
                if (sm->width > 0) winW = sm->width;
                if (sm->height > 0) winH = sm->height;
            }

            if (m_surfaces.find(surfId) == m_surfaces.end()) {
                SurfaceEntry entry{};
                entry.windowId = m_windowManager.createWindow(
                    title, winX, winY, winW, winH,
                    ::lcl::theme::UI::WindowTitleFocused);
                entry.width  = static_cast<uint32_t>(winW);
                entry.height = static_cast<uint32_t>(winH);
                entry.stride = entry.width * 4;
                m_surfaces[surfId] = entry;
                std::cout << "[LCL Compositor] Created Window (ID: " << entry.windowId
                          << ") for Surface " << surfId
                          << " from client PID " << msg.pid << "\n";
                m_needsRedraw = true;
            }

        // --- ATTACH_BUFFER: mmap the SCM_RIGHTS memfd into compositor address space ---
        } else if (isAttachBuffer) {
            uint32_t surfId = 1;
            uint32_t w = 540, h = 360, stride = 540 * 4;

            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgAttachBuffer)) {
                auto* bm = reinterpret_cast<const lcl::protocol::LCLMsgAttachBuffer*>(msg.payload.data());
                surfId = bm->surfaceId;
                w = bm->width;
                h = bm->height;
                stride = bm->stride > 0 ? bm->stride : w * 4;
            }

            int fd = msg.passedFd;
            if (fd >= 0) {
                size_t shmSize = static_cast<size_t>(stride) * h;
                void* pixels = mmap(nullptr, shmSize, PROT_READ, MAP_SHARED, fd, 0);
                if (pixels != MAP_FAILED) {
                    // Ensure surface exists
                    if (m_surfaces.find(surfId) == m_surfaces.end()) {
                        SurfaceEntry entry{};
                        int winX = DisplayScale::px(80);
                        int winY = DisplayScale::px(60);
                        entry.windowId = m_windowManager.createWindow(
                            "LCL Terminal", winX, winY, w, h,
                            ::lcl::theme::UI::WindowTitleFocused);
                        entry.width  = w;
                        entry.height = h;
                        entry.stride = stride;
                        m_surfaces[surfId] = entry;
                    }

                    auto& entry = m_surfaces[surfId];
                    if (entry.pixels && entry.shmSize > 0) {
                        munmap(entry.pixels, entry.shmSize);
                    }
                    entry.pixels  = pixels;
                    entry.shmSize = shmSize;
                    entry.width   = w;
                    entry.height  = h;
                    entry.stride  = stride;
                    entry.shmFd   = fd;
                    std::cout << "[LCL Compositor] Attached SHM Buffer (FD: " << fd
                              << ", " << w << "x" << h << ") for Surface " << surfId
                              << " from client PID " << msg.pid << "\n";
                } else {
                    std::cerr << "[DEBUG-COMP ERROR] mmap failed for memfd " << fd
                              << ": " << strerror(errno) << "\n";
                }
                m_needsRedraw = true;
            } else if (m_surfaces.find(surfId) != m_surfaces.end() && m_surfaces[surfId].pixels) {
                // Buffer commit notification for existing attached SHM buffer
                m_needsRedraw = true;
            }

        } else if (msg.command == "SPAWN_TERMINAL") {
            int x = DisplayScale::px(80);
            int y = DisplayScale::px(60);
            uint32_t winId = m_windowManager.createWindow(
                "LCL Terminal", x, y,
                DisplayScale::px(DisplayScale::kDefaultWinW),
                DisplayScale::px(DisplayScale::kDefaultWinH),
                ::lcl::theme::UI::WindowTitleFocused);
            std::cout << "[LCL Compositor] Created window surface (ID: " << winId << ") for client PID " << msg.pid << "\n";
            m_needsRedraw = true;
        }
    }
}

void Compositor::tickCursorBlink() {
    auto now = std::chrono::steady_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastBlinkCheck).count();
    if (ms >= 500) {
        m_lastBlinkCheck = now;
        m_needsRedraw = true;
    }
}

void Compositor::renderFrame() {
    if (!m_needsRedraw && !m_windowManager.isAnyWindowDirty()) return;

    // --- Build legacy WindowRenderContent list (text-based fallback for windows without SHM) ---
    std::vector<render::WindowRenderContent> contents;
    for (const auto& win : m_windowManager.getWindows()) {
        bool hasShmBuffer = false;
        for (const auto& [surfId, entry] : m_surfaces) {
            if (entry.windowId == win.id && entry.pixels) {
                hasShmBuffer = true;
                break;
            }
        }
        if (!hasShmBuffer) {
            render::WindowRenderContent c;
            c.windowId = win.id;
            c.lines = {
                "LCL OS Desktop",
                "Waiting for client surface buffer..."
            };
            c.cursorCol = 0;
            contents.push_back(c);
        }
    }

    // --- Begin Skia frame ---
    m_renderer.getSkiaRenderer()->beginFrame();

    // 1. Black desktop background
    m_renderer.clear(0xFF000000);

    // 2. Render windows with text-content fallback
    m_renderer.renderDesktop(m_windowManager, contents);

    // 3. Composite IPC client SHM framebuffers on top of their windows
    for (const auto& [surfId, entry] : m_surfaces) {
        if (!entry.pixels) continue;

        // Find the matching window to get its position
        const render::Window* win = nullptr;
        for (const auto& w : m_windowManager.getWindows()) {
            if (w.id == entry.windowId) { win = &w; break; }
        }
        if (!win) continue;

        // Draw SHM pixel buffer inside window content area (below title bar)
        using core::DisplayScale;
        int dstX = win->x;
        int dstY = win->y + DisplayScale::titleBarHeight();
        int srcW = static_cast<int>(entry.width);
        int srcH = static_cast<int>(entry.height);
        int stridePixels = static_cast<int>(entry.stride / 4);

        m_renderer.getSkiaRenderer()->drawBuffer(
            dstX, dstY, srcW, srcH,
            reinterpret_cast<const uint32_t*>(entry.pixels),
            stridePixels, 1.0f);
    }

    m_renderer.swapBuffers();
    m_windowManager.clearAllDirty();
    m_needsRedraw = false;
}

} // namespace lcl::core
