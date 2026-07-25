#include "core/compositor/compositor.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>

namespace lcl::core {

// ============================================================
// Construction / Destruction
// ============================================================

Compositor::Compositor() = default;

Compositor::~Compositor() {
    if (!m_initialized) return;

    // Shutdown in dependency-reverse order:
    // 1. Apps (send pending ACKs, kill PTYs)
    for (auto& app : m_apps) {
        sendAck(app->getAckClientFd());
        app->shutdown();
    }
    m_apps.clear();

    // 2. IPC — close socket before renderer/input tear-down
    m_ipcManager.shutdown();

    // 3. Renderer — release DRM framebuffer
    m_renderer.shutdown();

    // 4. Input — close evdev/libinput handles
    m_inputManager.shutdown();

    // 5. Display — release DRM/KMS lease
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
              << "[LCL Core] Initializing core subsystem skeleton...\n";

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

    // --- Window Manager ---
    m_windowManager.initialize(m_renderer.getWidth(), m_renderer.getHeight());

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(kCompositorSocket);

    // --- Wire input events → WindowManager + focused app ---
    m_inputManager.setEventCallback([this](const InputEvent& ev) {
        if (m_windowManager.processInputEvent(ev)) {
            m_needsRedraw = true;
        }
        const auto& wins = m_windowManager.getWindows();
        if (!wins.empty()) {
            uint32_t focusedId = wins.back().id;
            for (auto& app : m_apps) {
                if (static_cast<uint32_t>(app->getWindowId()) == focusedId) {
                    app->handleInput(ev);
                    m_needsRedraw = true;
                    break;
                }
            }
        }
    });

    // --- Launch primary desktop session (Terminal App) ---
    auto primary = std::make_unique<apps::TerminalApp>();
    m_startupManager.launchDefaultSession(m_windowManager, *primary);
    m_apps.push_back(std::move(primary));

    // --- Frame pacing from DRM refresh rate ---
    uint32_t hz = (m_displayManager.isInitialized()
                   ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (hz == 0) hz = 60;
    m_targetFrameDuration = std::chrono::microseconds(1000000 / hz);

    std::cout << "[LCL Core] Secure Unix Domain Socket Compositor IPC active! "
                 "Press Ctrl+C to terminate.\n"
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
        syncWindowLifecycle();
        updateApps();
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
    // Cleanup happens in ~Compositor()
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
        std::cout << "[LCL IPC SECURITY] Message from authenticated Client (PID: "
                  << msg.pid << ", UID: " << msg.uid << "): '" << msg.command << "'\n";

        if (msg.command == "SPAWN_TERMINAL") {
            handleSpawnTerminal(msg, /*waitMode=*/false);
        } else if (msg.command == "SPAWN_TERMINAL_WAIT") {
            handleSpawnTerminal(msg, /*waitMode=*/true);
        } else if (msg.command == "DESTROY_LAST_WINDOW") {
            handleDestroyLastWindow();
        }
    }
}

void Compositor::syncWindowLifecycle() {
    // Remove apps whose window was closed via the UI close button
    const auto& activeWins = m_windowManager.getWindows();
    for (auto it = m_apps.begin(); it != m_apps.end(); ) {
        uint32_t winId = static_cast<uint32_t>((*it)->getWindowId());
        bool exists = std::any_of(activeWins.begin(), activeWins.end(),
                                  [winId](const render::Window& w) { return w.id == winId; });
        if (!exists) {
            std::cout << "[LCL Compositor] Window ID " << winId
                      << " closed via UI button. Destroying process.\n";
            sendAck((*it)->getAckClientFd());
            (*it)->shutdown();
            it = m_apps.erase(it);
            m_needsRedraw = true;
        } else {
            ++it;
        }
    }
}

void Compositor::updateApps() {
    for (auto it = m_apps.begin(); it != m_apps.end(); ) {
        auto& app = *it;

        // Poll PTY output; mark window dirty if new content arrived
        if (app->update()) {
            uint32_t winId = static_cast<uint32_t>(app->getWindowId());
            for (auto& w : m_windowManager.getWindowsMutable()) {
                if (w.id == winId) { w.markDirty(); break; }
            }
            m_needsRedraw = true;
        }

        // Remove apps whose shell process has exited
        if (!app->isAlive()) {
            uint32_t winId = static_cast<uint32_t>(app->getWindowId());
            std::cout << "[LCL Compositor] Terminal App (Window ID: " << winId
                      << ") process exited.\n";
            sendAck(app->getAckClientFd());
            m_windowManager.removeWindow(winId);
            app->shutdown();
            it = m_apps.erase(it);
            m_needsRedraw = true;
        } else {
            ++it;
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

    std::vector<render::WindowRenderContent> contents;
    contents.reserve(m_apps.size());
    for (const auto& app : m_apps) {
        contents.push_back(app->getRenderContent());
    }

    m_renderer.renderDesktop(m_windowManager, contents);
    m_renderer.swapBuffers();
    m_windowManager.clearAllDirty();
    m_needsRedraw = false;
}

// ============================================================
// IPC Command Handlers
// ============================================================

void Compositor::handleSpawnTerminal(const IPCClientMessage& msg, bool waitMode) {
    int x = DisplayScale::px(80 + static_cast<int>(m_apps.size() * 40));
    int y = DisplayScale::px(60 + static_cast<int>(m_apps.size() * 30));

    uint32_t winId = m_windowManager.createWindow(
        "LCL Terminal", x, y,
        DisplayScale::px(DisplayScale::kDefaultWinW),
        DisplayScale::px(DisplayScale::kDefaultWinH),
        lcl::theme::UI::WindowTitleFocused);

    auto app = std::make_unique<apps::TerminalApp>();
    app->initialize(winId);
    if (waitMode) {
        app->setAckClientFd(msg.clientFd);
    }

    std::cout << "[LCL Compositor] Dynamically spawned GUI Terminal Window (ID: "
              << winId << (waitMode ? " [blocking -w mode]" : "") << ") on desktop!\n";

    m_apps.push_back(std::move(app));
    m_needsRedraw = true;
}

void Compositor::handleDestroyLastWindow() {
    if (m_apps.size() > 1) {
        auto& last = m_apps.back();
        uint32_t winId = static_cast<uint32_t>(last->getWindowId());
        std::cout << "[LCL Compositor] DESTROY_LAST_WINDOW: Destroying Window ID " << winId
                  << " and terminating process group due to SIGINT cancel.\n";
        m_windowManager.removeWindow(winId);
        last->shutdown();
        m_apps.pop_back();
        m_needsRedraw = true;
    }
}

// ============================================================
// Helpers
// ============================================================

void Compositor::sendAck(int fd) {
    if (fd >= 0) {
        IPCManager::sendResponse(fd, "DONE");
    }
}

} // namespace lcl::core
