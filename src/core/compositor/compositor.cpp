#include "core/compositor/compositor.hpp"
#include "platform/common/output_scale.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace lcl::core {

namespace {

bool environmentEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' && value[0] != '0';
}

} // namespace

// ============================================================
// Construction / Destruction
// ============================================================

Compositor::Compositor(lcl::platform::IPlatformServices& platformServices)
    : m_platformServices(platformServices) {}

Compositor::~Compositor() {
    if (!m_initialized) return;

    // 1. IPC — close socket before renderer tear-down
    m_ipcManager.shutdown();

    // 2. Renderer — release DRM/GPU surface resources
    m_renderer.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: "
              << m_loopTicks << "\n";
}

// ============================================================
// Initialization
// ============================================================

bool Compositor::initialize() {
    if (m_initialized) return true;

    signal(SIGPIPE, SIG_IGN);
    m_showFpsOverlay = environmentEnabled("LCL_DEBUG_OVERLAY");

    std::cout << "====================================================\n"
              << "  LCL Core Linux (LCL) v0.1.0 - Core Engine\n"
              << "  Architecture: Direct DRM/KMS & evdev (No X11/Wayland)\n"
              << "  C++ Standard: C++20\n"
              << "====================================================\n"
              << "[LCL Core] Initializing pure Display Server compositor...\n";

    auto& display = m_platformServices.display();
    auto& graphics = m_platformServices.graphics();
    auto& input = m_platformServices.input();
    const auto& paths = m_platformServices.paths();

    // --- Renderer ---
    const auto& mode = display.activeMode();
    uint32_t renW = mode.width > 0 ? mode.width : 1024;
    uint32_t renH = mode.height > 0 ? mode.height : 768;
    if (!m_renderer.initialize(renW, renH, &graphics, &display, nullptr)) {
        std::cout << "[LCL Core] Renderer running in fallback mode.\n";
    }

    // --- Window Manager Canvas ---
    const float outputScale = lcl::platform::sanitizeOutputScale(mode.scaleFactor);
    std::cout << "[LCL Output] Device scale: " << outputScale << "\n";
    m_renderer.getRasterRenderer()->setDeviceScale(outputScale);
    display.initHardwareCursor(64, 64, outputScale);
    m_windowManager.initialize(
        static_cast<float>(m_renderer.getWidth()) / outputScale,
        static_cast<float>(m_renderer.getHeight()) / outputScale);
    m_protocolDispatcher = std::make_unique<ProtocolDispatcher>(
        m_renderer, m_windowManager, m_surfaces, m_sceneRegistry,
        m_focusController, m_shellStateBroker);

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(paths.compositorSocketPath());

    // --- Route input through the dedicated focus/hit-test bridge ---
    m_inputRouter = std::make_unique<InputRouter>(
        m_windowManager, m_surfaces, m_sceneRegistry, outputScale);
    input.initialize([this](const lcl::platform::RawInputEvent& event) {
        if (m_inputRouter && m_inputRouter->route(event)) {
            m_needsRedraw = true;
            m_shellStateDirty = true;
        }
    });

    // --- Frame pacing from display refresh rate ---
    uint32_t hz = (display.isInitialized() && mode.refreshRate > 0)
                   ? mode.refreshRate : 60;
    m_refreshIntervalNs = 1000000000ull / hz;
    m_inputRouter->setRefreshInterval(std::chrono::nanoseconds(m_refreshIntervalNs));

    std::cout << "[LCL Core] Pure Display Server active on " << paths.compositorSocketPath()
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
    if (m_platformServices.input().isInitialized()) {
        m_platformServices.input().pollEvents(
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

    // 1. Query dynamic monitor refresh rate (default to 60Hz if undetected)
    const auto& mode = m_platformServices.display().activeMode();
    uint32_t refreshHz = (m_platformServices.display().isInitialized() && mode.refreshRate > 0)
                         ? mode.refreshRate : 60;

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
            if (m_inputRouter) {
                m_inputRouter->syncWindowState();
            }
            m_needsRedraw = true;
            m_shellStateDirty = true;
        }
        processIPC();
        // Refresh-cadenced Live configures may have been deferred while an
        // input batch arrived. Revisit the coalesced newest geometry once per
        // compositor loop even when the pointer becomes stationary.
        if (m_inputRouter) m_inputRouter->syncWindowState();
        synchronizeShellState();
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

    auto* raster = m_renderer.getRasterRenderer();
    const float deviceScale = raster->getDeviceScale();
    const float screenW = static_cast<float>(m_renderer.getWidth()) / deviceScale;
    constexpr float cardW = 220.0f;
    constexpr float cardH = 88.0f;
    const float cardX = screenW - cardW - 16.0f;
    constexpr float cardY = 16.0f;

    graphics::DisplayListBuilder builder;
    graphics::Path card;
    card.addRRect({{cardX, cardY, cardW, cardH}, 10.0f, 10.0f, 2.0f});
    graphics::Paint fill;
    fill.color = {15, 23, 42, 221};
    builder.drawPath(card, fill);
    graphics::Paint stroke;
    stroke.color = {56, 189, 248, 102};
    stroke.style = graphics::PaintStyle::Stroke;
    stroke.stroke.width = 1.0f;
    builder.drawPath(card, stroke);

    // Determine engine label from platform graphics context
    std::string engineStr = "Engine: ";
    auto& graphics = m_platformServices.graphics();
    if (graphics.isInitialized()) {
        engineStr += graphics.isHardwareAccelerated() ? "Hardware Accelerated" : "Software Fallback";
    } else {
        engineStr += "Software Fallback";
    }

    // Determine VSync label
    std::string vsyncStr = "VSync: ";
    if (graphics.isInitialized() && graphics.presentsToDisplay()) {
        vsyncStr += "ON";
    } else {
        vsyncStr += "OFF";
    }

    char fpsBuf[64];
    graphics::Color fpsColor{74, 222, 128, 255};
    if (m_currentFps <= 0.0f) {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: 0 (Idle)");
        fpsColor = {148, 163, 184, 255};
    } else {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.0f (%.1f ms)", m_currentFps, m_currentFrameMs);
    }

    const float textX = cardX + 12.0f;
    const float textY = cardY + 10.0f;
    constexpr float lineSpacing = 18.0f;
    constexpr float fontSize = 16.0f;

    builder.drawText({textX, textY}, fpsBuf, fpsColor, fontSize);
    builder.drawText({textX, textY + lineSpacing}, engineStr,
                     {56, 189, 248, 255}, fontSize);
    builder.drawText({textX, textY + lineSpacing * 2.0f}, vsyncStr,
                     {52, 211, 153, 255}, fontSize);
    char composeBuf[64];
    std::snprintf(composeBuf, sizeof(composeBuf), "Compose: %.2f ms", m_lastComposeMs);
    builder.drawText({textX, textY + lineSpacing * 3.0f}, composeBuf,
                     {250, 204, 21, 255}, fontSize);
    raster->replayDisplayList(
        builder.build(),
        {{static_cast<float>(m_renderer.getWidth()) / deviceScale,
          static_cast<float>(m_renderer.getHeight()) / deviceScale},
         {m_renderer.getWidth(), m_renderer.getHeight()}, deviceScale});
}

void Compositor::renderFrame() {
    if (!m_needsRedraw && !m_windowManager.isAnyWindowDirty()) return;

    const bool hasActiveTransitions = m_frameScheduler.advanceTransitions(m_surfaces);
    const bool hasPendingGpuRelease = std::any_of(
        m_surfaces.begin(), m_surfaces.end(), [](const auto& pair) {
            return !pair.second.pendingDmaBufReleases.empty();
        });
    const int releaseFenceFd = hasPendingGpuRelease
        ? m_renderer.createNativeFence() : -1;
    for (auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.pendingMinimize) {
            m_windowManager.minimizeWindow(entry.windowId);
            entry.pendingMinimize = false;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
        }
        if (entry.rollbackRequested) {
            m_windowManager.rollbackWindowGeometry(
                entry.windowId,
                {entry.rollbackX, entry.rollbackY, entry.rollbackWidth, entry.rollbackHeight},
                entry.rollbackWasMaximized, entry.rollbackWasMinimized,
                entry.resizeGeometryGeneration);
            entry.rollbackRequested = false;
            entry.resizeGeometryGeneration = 0;
        }
    }
    const auto surfaces = m_surfaces.snapshot();
    const auto composeStart = std::chrono::steady_clock::now();
    m_compositorRenderer.render(
        m_renderer, m_platformServices.display(), m_windowManager, surfaces,
        [this] { renderDiagnosticOverlay(); },
        !hasActiveTransitions && !m_showFpsOverlay);
    m_lastComposeMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - composeStart).count();

    // The frame that stopped referencing these textures has now been handed
    // to EGL/KMS. Destroy the compositor import and tell the producer that
    // its GBM pool slot is reusable. Mesa's DMA-BUF implicit fence is the
    // synchronization contract for this first zero-copy transport revision.
    for (auto& [surfaceKey, entry] : m_surfaces) {
        for (const auto& release : entry.pendingDmaBufReleases) {
            if (release.texture != 0) {
                m_renderer.getRasterRenderer()->releaseDmaBufTexture(release.texture);
            }
            if (entry.clientFd >= 0 && release.bufferId != 0) {
                protocol::LCLHeader header{};
                header.opcode = protocol::LCLOpcode::ReleaseDmaBuf;
                header.payloadSize = sizeof(protocol::LCLMsgReleaseDmaBuf);
                protocol::LCLMsgReleaseDmaBuf message{};
                message.surfaceId = static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu);
                message.bufferId = release.bufferId;
                protocol::sendMsgWithFd(
                    entry.clientFd, header, &message, releaseFenceFd);
            }
        }
        entry.pendingDmaBufReleases.clear();
    }
    if (releaseFenceFd >= 0) close(releaseFenceFd);

    const uint64_t presentedAtNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.clientFd < 0 || !entry.hasCommittedBuffer ||
            !SurfaceRegistry::hasUnpresentedFrame(entry)) continue;
        protocol::LCLHeader header{};
        header.opcode = protocol::LCLOpcode::FramePresented;
        header.payloadSize = sizeof(protocol::LCLMsgFramePresented);
        protocol::LCLMsgFramePresented message{};
        message.surfaceId = static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu);
        message.timestampNs = presentedAtNs;
        message.refreshIntervalNs = m_refreshIntervalNs;
        if (protocol::sendMsgWithFd(entry.clientFd, header, &message)) {
            SurfaceRegistry::completePresentation(entry);
        }
    }

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
        if (found != m_surfaces.end()) {
            // The client was already asked to destroy this surface, so it no
            // longer needs a ReleaseDmaBuf message. It does need its imported
            // textures released before the registry entry disappears.
            SurfaceRegistry::releaseBuffer(found->second);
            for (const auto& release : found->second.pendingDmaBufReleases) {
                if (release.texture != 0) {
                    m_renderer.getRasterRenderer()->releaseDmaBufTexture(release.texture);
                }
            }
            found->second.pendingDmaBufReleases.clear();
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
