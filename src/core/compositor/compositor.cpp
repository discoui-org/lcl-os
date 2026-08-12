#include "core/compositor/compositor.hpp"
#include "core/display/display_scale.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>

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
    m_protocolDispatcher = std::make_unique<ProtocolDispatcher>(
        m_renderer, m_windowManager, m_surfaces, m_sceneRegistry,
        m_focusController, m_shellStateBroker);

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(kCompositorSocket);

    // --- Route input through the dedicated focus/hit-test bridge ---
    m_inputRouter = std::make_unique<InputRouter>(m_windowManager, m_surfaces, m_sceneRegistry);
    m_inputManager.setEventCallback([this](const InputEvent& event) {
        if (m_inputRouter && m_inputRouter->route(event)) {
            m_needsRedraw = true;
            m_shellStateDirty = true;
        }
    });

    // --- Frame pacing from DRM refresh rate ---
    uint32_t hz = (m_displayManager.isInitialized()
                   ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (hz == 0) hz = 60;

    std::cout << "[LCL Core] Pure Display Server active on " << kCompositorSocket
              << ". Listening for client surface registrations.\n"
              << "[LCL Core] High Refresh Rate active: targeting " << hz
              << " Hz (~" << (1000000 / hz) << " us per frame budget).\n";

    m_frameScheduler.reset();
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
    if (m_protocolDispatcher && m_protocolDispatcher->process(m_ipcManager)) {
        if (m_inputRouter) {
            m_inputRouter->syncWindowState();
        }
        m_needsRedraw = true;
        m_shellStateDirty = true;
    }
}

void Compositor::synchronizeShellState() {
    if (!m_shellStateDirty) return;
    m_sceneRegistry.reconcileWindowState(m_windowManager);
    for (const auto& change : m_sceneRegistry.takePendingChanges()) {
        m_shellStateBroker.publish(change);
    }
    if (const auto focus = m_focusController.reconcile(m_sceneRegistry, m_windowManager)) {
        m_shellStateBroker.publish(*focus);
    }
    if (m_protocolDispatcher) {
        m_protocolDispatcher->publishShellStateToSubscribers();
    }
    m_shellStateDirty = false;
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
    const auto targetFrameDuration = m_frameScheduler.frameBudgetForHz(refreshHz);

    std::cout << "[LCL Core] Dynamic Frame Pacer Active: " << refreshHz << " Hz "
              << "(Target Period: " << targetPeriodMs << " ms, Headroom: " << headroomMs
              << " ms, Budget: " << targetBudgetMs << " ms)\n";

    m_lastFpsTime = std::chrono::steady_clock::now();

    while (m_running.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        processInput();
        if (m_windowManager.updateAnimations()) {
            m_needsRedraw = true;
            m_shellStateDirty = true;
        }
        processIPC();
        synchronizeShellState();
        if (m_frameScheduler.cursorBlinkDue()) {
            m_needsRedraw = true;
        }

        bool willDraw = m_needsRedraw || m_windowManager.isAnyWindowDirty();

        if (willDraw) {
            renderFrame();

            m_frameScheduler.waitForFrame(frameStart, targetFrameDuration);
        } else {
            m_frameScheduler.waitIdle();
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

    const bool hasActiveTransitions = m_frameScheduler.advanceTransitions(m_surfaces);
    const auto surfaces = m_surfaces.snapshot();
    m_compositorRenderer.render(m_renderer, m_displayManager, m_windowManager, surfaces);

    std::vector<SurfaceRegistry::Key> surfacesToRemove;
    for (const auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.pendingDestroy) {
            surfacesToRemove.push_back(surfaceKey);
        }
    }
    for (const auto surfaceKey : surfacesToRemove) {
        const auto found = m_surfaces.find(surfaceKey);
        if (found != m_surfaces.end() && found->second.windowId > 0) {
            m_windowManager.removeWindow(found->second.windowId);
        }
        m_sceneRegistry.removeSurface(surfaceKey);
        m_surfaces.erase(surfaceKey);
        m_shellStateDirty = true;
    }

    synchronizeShellState();

    m_windowManager.clearAllDirty();
    m_needsRedraw = hasActiveTransitions || !surfacesToRemove.empty();
    ++m_fpsFrameCount;
}


} // namespace lcl::core
