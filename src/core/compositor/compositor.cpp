#include "core/compositor/compositor.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sys/mman.h>
#include <csignal>

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

    signal(SIGPIPE, SIG_IGN);

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

    // --- Wire input events → WindowManager & focused IPC client ---
    m_inputManager.setEventCallback([this](const InputEvent& ev) {
        bool stateChanged = m_windowManager.processInputEvent(ev);
        if (stateChanged) {
            m_needsRedraw = true;

            // Notify surface clients if window bounds changed during resize
            for (const auto& win : m_windowManager.getWindows()) {
                for (const auto& [surfId, entry] : m_surfaces) {
                    if (entry.windowId == win.id && entry.clientFd >= 0) {
                        uint32_t contentW = static_cast<uint32_t>(win.pendingWidth > 0 ? win.pendingWidth : win.width);
                        uint32_t contentH = static_cast<uint32_t>(std::max(1, (win.pendingHeight > 0 ? win.pendingHeight : win.height) - DisplayScale::titleBarHeight()));
                        if (contentW != entry.width || contentH != entry.height) {
                            protocol::LCLHeader header{};
                            header.opcode = protocol::LCLOpcode::ConfigureBounds;
                            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

                            protocol::LCLMsgConfigureBounds cfgMsg{};
                            cfgMsg.surfaceId = surfId;
                            cfgMsg.x = win.x;
                            cfgMsg.y = win.y;
                            cfgMsg.width = contentW;
                            cfgMsg.height = contentH;
                            cfgMsg.isFocused = win.isFocused ? 1 : 0;

                            protocol::sendMsgWithFd(entry.clientFd, header, &cfgMsg);
                        }
                        break;
                    }
                }
            }
        }

        if (ev.type == InputEventType::KeyboardKey) {
            uint32_t focusedWinId = m_windowManager.getFocusedWindowId();
            if (focusedWinId > 0) {
                for (const auto& [surfId, entry] : m_surfaces) {
                    if (entry.windowId == focusedWinId && entry.clientFd >= 0) {
                        protocol::LCLHeader header{};
                        header.opcode = protocol::LCLOpcode::InputEvent;
                        header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

                        protocol::LCLMsgInputEvent inputMsg{};
                        inputMsg.surfaceId = surfId;
                        inputMsg.type = 1; // KeyboardKey
                        inputMsg.key = ev.key;
                        inputMsg.pressed = ev.pressed ? 1 : 0;

                        protocol::sendMsgWithFd(entry.clientFd, header, &inputMsg);
                        break;
                    }
                }
            }
        } else if (ev.type == InputEventType::PointerMotion || ev.type == InputEventType::PointerButton) {
            uint32_t focusedWinId = m_windowManager.getFocusedWindowId();
            if (focusedWinId > 0) {
                const render::Window* targetWin = nullptr;
                for (const auto& win : m_windowManager.getWindows()) {
                    if (win.id == focusedWinId) {
                        targetWin = &win;
                        break;
                    }
                }
                if (targetWin) {
                    for (const auto& [surfId, entry] : m_surfaces) {
                        if (entry.windowId == focusedWinId && entry.clientFd >= 0) {
                            int titleOffset = (targetWin->decorationMode == render::DecorationMode::SSD)
                                              ? DisplayScale::titleBarHeight() : 0;
                            float localX = static_cast<float>(m_windowManager.getMouseX() - targetWin->x);
                            float localY = static_cast<float>(m_windowManager.getMouseY() - targetWin->y - titleOffset);

                            protocol::LCLHeader header{};
                            header.opcode = protocol::LCLOpcode::InputEvent;
                            header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

                            protocol::LCLMsgInputEvent inputMsg{};
                            inputMsg.surfaceId = surfId;
                            inputMsg.type = (ev.type == InputEventType::PointerMotion) ? 2 : 3;
                            inputMsg.x = localX;
                            inputMsg.y = localY;
                            inputMsg.key = ev.button;
                            inputMsg.pressed = ev.pressed ? 1 : 0;

                            protocol::sendMsgWithFd(entry.clientFd, header, &inputMsg);
                            break;
                        }
                    }
                }
            }
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

        bool isDisconnect   = (msg.header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) ||
                               (msg.command.rfind("CLIENT_DISCONNECT", 0) == 0);

        if (isDisconnect) {
            std::vector<uint64_t> surfacesToRemove;
            for (auto& [surfKey, entry] : m_surfaces) {
                if (entry.clientFd == msg.clientFd || (msg.pid > 0 && (surfKey >> 32) == static_cast<uint64_t>(msg.pid))) {
                    if (entry.windowId > 0) {
                        std::cout << "[LCL Compositor] Removing Window ID: " << entry.windowId
                                  << " for disconnected client FD: " << msg.clientFd << "\n";
                        m_windowManager.removeWindow(entry.windowId);
                    }
                    if (entry.pixels && entry.shmSize > 0) {
                        munmap(entry.pixels, entry.shmSize);
                        entry.pixels = nullptr;
                    }
                    if (entry.shmFd >= 0) {
                        close(entry.shmFd);
                        entry.shmFd = -1;
                    }
                    surfacesToRemove.push_back(surfKey);
                }
            }
            for (uint64_t key : surfacesToRemove) {
                m_surfaces.erase(key);
            }
            m_needsRedraw = true;
        } else if (isSurfaceCreate) {
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

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                SurfaceEntry entry{};
                int frameW = winW;
                int frameH = winH + DisplayScale::titleBarHeight();
                entry.windowId = m_windowManager.createWindow(
                    title, winX, winY, frameW, frameH,
                    ::lcl::theme::UI::WindowTitleFocused);
                entry.clientFd = msg.clientFd;
                entry.width  = static_cast<uint32_t>(winW);
                entry.height = static_cast<uint32_t>(winH);
                entry.stride = entry.width * 4;
                m_surfaces[surfaceKey] = entry;
                std::cout << "[LCL Compositor] Created Window (ID: " << entry.windowId
                          << ") for Surface " << surfId
                          << " from client PID " << msg.pid << "\n";
                m_needsRedraw = true;
            } else {
                m_surfaces[surfaceKey].clientFd = msg.clientFd;
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

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                static int spawnIndex = 0;
                int winX = DisplayScale::px(80 + (spawnIndex % 6) * 30);
                int winY = DisplayScale::px(60 + (spawnIndex % 6) * 30);
                spawnIndex++;

                SurfaceEntry entry{};
                int frameW = static_cast<int>(w);
                int frameH = static_cast<int>(h) + DisplayScale::titleBarHeight();
                entry.windowId = m_windowManager.createWindow(
                    "LCL Terminal", winX, winY, frameW, frameH,
                    ::lcl::theme::UI::WindowTitleFocused);
                entry.clientFd = msg.clientFd;
                entry.width  = w;
                entry.height = h;
                entry.stride = stride;
                m_surfaces[surfaceKey] = entry;
            }

            auto& entry = m_surfaces[surfaceKey];
            entry.clientFd = msg.clientFd;

            int fd = msg.passedFd;
            if (fd >= 0) {
                size_t shmSize = static_cast<size_t>(stride) * h;
                if (entry.pixels && entry.shmSize == shmSize && entry.width == w && entry.height == h) {
                    // Buffer is ALREADY mapped in compositor address space with identical size & dimensions!
                    // Do NOT unmap/remap memory on every frame to avoid rendering race conditions.
                    close(fd);
                } else {
                    void* pixels = mmap(nullptr, shmSize, PROT_READ, MAP_SHARED, fd, 0);
                    if (pixels != MAP_FAILED) {
                        if (entry.pixels && entry.shmSize > 0) {
                            munmap(entry.pixels, entry.shmSize);
                        }
                        if (entry.shmFd >= 0 && entry.shmFd != fd) {
                            close(entry.shmFd);
                        }
                        entry.pixels  = pixels;
                        entry.shmSize = shmSize;
                        entry.shmFd   = fd;
                        entry.width   = w;
                        entry.height  = h;
                        entry.stride  = stride;
                    } else {
                        std::cerr << "[LCL Compositor ERROR] mmap failed for memfd " << fd
                                  << ": " << strerror(errno) << "\n";
                        close(fd);
                    }
                }
            } else {
                entry.width  = w;
                entry.height = h;
                entry.stride = stride;
            }

            // Notify WindowManager of client surface buffer commit
            int frameW = static_cast<int>(w);
            int frameH = static_cast<int>(h) + DisplayScale::titleBarHeight();
            m_windowManager.commitSurfaceGeometry(entry.windowId, frameW, frameH);
            m_needsRedraw = true;

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetDecorationMode ||
                   msg.command.rfind("SET_DECORATION_MODE", 0) == 0) {
            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | 1;
            auto it = m_surfaces.find(surfaceKey);
            if (it != m_surfaces.end()) {
                lcl::protocol::LCLDecorationMode mode = lcl::protocol::LCLDecorationMode::SSD;
                if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetDecorationMode)) {
                    auto* decMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetDecorationMode*>(msg.payload.data());
                    mode = decMsg->mode;
                } else if (msg.command.find("CSD") != std::string::npos) {
                    mode = lcl::protocol::LCLDecorationMode::CSD;
                }
                render::DecorationMode wmMode = (mode == lcl::protocol::LCLDecorationMode::CSD) ?
                    render::DecorationMode::CSD : render::DecorationMode::SSD;
                m_windowManager.setDecorationMode(it->second.windowId, wmMode);
                std::cout << "[LCL Compositor] Set decoration mode for Window " << it->second.windowId
                          << " to " << (wmMode == render::DecorationMode::CSD ? "CSD" : "SSD") << "\n";
                m_needsRedraw = true;
            }

        } else if (msg.command == "SPAWN_TERMINAL" || msg.command.rfind("SPAWN_TERMINAL", 0) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                // Child process: execute lcl-terminal binary
                execl("/home/user/Applications/Terminal.app/bin/lcl-terminal", "lcl-terminal", nullptr);
                execl("/bin/lcl-terminal", "lcl-terminal", nullptr);
                execl("/usr/bin/lcl-terminal", "lcl-terminal", nullptr);
                _exit(1);
            } else if (pid > 0) {
                std::cout << "[LCL Compositor] Spawned new LCL Terminal process (PID: " << pid << ")\n";
            } else {
                std::cerr << "[LCL Compositor ERROR] Failed to fork process for SPAWN_TERMINAL.\n";
            }
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

void Compositor::run() {
    if (!m_initialized) return;

    // 1. Query dynamic DRM/KMS monitor refresh rate (default to 60Hz if undetected)
    uint32_t refreshHz = (m_displayManager.isInitialized() ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (refreshHz == 0) refreshHz = 60;

    // 2. Calculate dynamic frame period & headroom allowance (~0.5ms safety budget)
    float targetPeriodMs = 1000.0f / static_cast<float>(refreshHz);
    float headroomMs = std::min(0.5f, targetPeriodMs * 0.08f);
    float targetBudgetMs = targetPeriodMs - headroomMs;
    auto targetFrameDuration = std::chrono::microseconds(static_cast<int64_t>(targetBudgetMs * 1000.0f));

    std::cout << "[LCL Core] Dynamic Frame Pacer Active: " << refreshHz << " Hz "
              << "(Target Period: " << targetPeriodMs << " ms, Headroom: " << headroomMs
              << " ms, Budget: " << targetBudgetMs << " ms)\n";

    m_lastFpsTime = std::chrono::steady_clock::now();

    while (m_running.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        processInput();
        processIPC();
        tickCursorBlink();

        bool willDraw = m_needsRedraw || m_windowManager.isAnyWindowDirty();

        if (willDraw) {
            renderFrame();

            // Dynamic Frame Pacing with Headroom Allowance to prevent frame skipping & uncapped rendering
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - frameStart);
            if (elapsed < targetFrameDuration) {
                std::this_thread::sleep_for(targetFrameDuration - elapsed);
            }
        } else {
            // Idle State: Sleep 2ms to prevent CPU busy-spinning and reduce idle usage to ~0%
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Sliding 1.0s window FPS calculation (displays 0 (Idle) when no frames were rendered)
        auto fpsNow = std::chrono::steady_clock::now();
        float elapsedSec = std::chrono::duration<float>(fpsNow - m_lastFpsTime).count();
        if (elapsedSec >= 1.0f) {
            if (m_fpsFrameCount == 0) {
                m_currentFps = 0.0f;
                m_currentFrameMs = 0.0f;
            } else {
                m_currentFps = static_cast<float>(m_fpsFrameCount) / elapsedSec;
                m_currentFrameMs = (elapsedSec * 1000.0f) / static_cast<float>(m_fpsFrameCount);
            }
            m_fpsFrameCount = 0;
            m_lastFpsTime = fpsNow;
        }

        ++m_loopTicks;
    }
}

void Compositor::renderDiagnosticOverlay() {
    if (!m_showFpsOverlay) return;

    int screenW = static_cast<int>(m_renderer.getWidth());
    int cardW = DisplayScale::px(220);
    int cardH = DisplayScale::px(70);
    int cardX = screenW - cardW - DisplayScale::px(16);
    int cardY = DisplayScale::px(16);

    // Render translucent dark slate card background with sky accent border
    m_renderer.drawFilledRect(cardX, cardY, cardW, cardH, 0xDD0F172A);
    m_renderer.drawRect(cardX, cardY, cardW, cardH, 0x6638BDF8);

    // Determine engine label from audited EGL backend renderer string
    std::string engineStr = "Engine: ";
    auto* egl = m_displayManager.getEGLBackend();
    if (egl && egl->isInitialized()) {
        engineStr += egl->getGLRendererString();
    } else {
        engineStr += "Software Fallback";
    }

    // Determine VSync label
    std::string vsyncStr = "VSync: ";
    if (egl && egl->isInitialized() && egl->isVSyncActive()) {
        vsyncStr += "ON";
    } else {
        vsyncStr += "OFF";
    }

    char fpsBuf[64];
    uint32_t fpsColor = 0xFF4ADE80; // Bright Lime
    if (m_currentFps <= 0.0f) {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: 0 (Idle)");
        fpsColor = 0xFF94A3B8; // Slate Gray when idle
    } else {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.0f (%.1f ms)", m_currentFps, m_currentFrameMs);
    }

    int textX = cardX + DisplayScale::px(12);
    int textY = cardY + DisplayScale::px(10);
    int lineSpacing = DisplayScale::px(18);

    m_renderer.drawString(textX, textY, fpsBuf, fpsColor);
    m_renderer.drawString(textX, textY + lineSpacing, engineStr, 0xFF38BDF8);     // Cyan Engine
    m_renderer.drawString(textX, textY + lineSpacing * 2, vsyncStr, 0xFF34D399); // Emerald VSync
}

void Compositor::renderFrame() {
    if (!m_needsRedraw && !m_windowManager.isAnyWindowDirty()) return;

    // --- Build fallback content list for windows without active SHM buffers ---
    std::vector<render::WindowRenderContent> contents;
    for (const auto& win : m_windowManager.getWindows()) {
        bool hasShmBuffer = false;
        for (const auto& [surfKey, entry] : m_surfaces) {
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

    // 1. Clear Desktop Canvas (Black background)
    m_renderer.clear(0xFF000000);

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    using core::DisplayScale;
    for (const auto& win : m_windowManager.getWindows()) {
        // A. Render Server-Side Window Frame (Titlebar & Border) if SSD enabled
        if (win.decorationMode == render::DecorationMode::SSD) {
            m_renderer.drawWindowFrame(win.x, win.y, win.width, win.height, win.title, win.headerColor);
        }

        // B. Find matching client SHM surface buffer for this window
        const SurfaceEntry* matchingSurface = nullptr;
        for (const auto& [surfKey, entry] : m_surfaces) {
            if (entry.windowId == win.id && entry.pixels) {
                matchingSurface = &entry;
                break;
            }
        }

        if (matchingSurface) {
            int titleOffset = (win.decorationMode == render::DecorationMode::SSD) ? DisplayScale::titleBarHeight() : 0;
            int dstX = win.x;
            int dstY = win.y + titleOffset;
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);

            m_renderer.getSkiaRenderer()->drawBuffer(
                dstX, dstY, srcW, srcH,
                reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                stridePixels, 1.0f);
        } else {
            // Render text fallback content if SHM buffer is not yet attached
            const render::WindowRenderContent* content = nullptr;
            for (const auto& c : contents) {
                if (c.windowId == win.id) { content = &c; break; }
            }
            if (content) {
                m_renderer.renderWindowContent(win, content);
            }
        }
    }

    // 3. Render Diagnostic FPS Overlay
    renderDiagnosticOverlay();

    // 4. Render Mouse Cursor on top of all windows
    m_renderer.drawCursor(m_windowManager.getMouseX(), m_windowManager.getMouseY());

    m_renderer.swapBuffers();
    m_windowManager.clearAllDirty();
    m_needsRedraw = false;

    // Increment presented frame count (used by 1.0s sliding window in run loop)
    m_fpsFrameCount++;
}

} // namespace lcl::core
