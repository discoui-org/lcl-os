#include "system/compositor/compositor.hpp"
#include "system/compositor/surface_transaction_coordinator.hpp"
#include "platforms/common/output_scale.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    : m_platformServices(platformServices),
      m_peerAuthenticator({
          .identities = {
              .registryPath = platformServices.paths().applicationIdentityRegistryPath(),
              .firstAppUid = 61000,
              .lastAppUid = 61999,
              .ownerUid = 0,
              .ownerGid = 0,
          },
          .launches = {
              .directoryPath = platformServices.paths().applicationLaunchRegistryPath(),
              .firstAppUid = 61000,
              .lastAppUid = 61999,
              .ownerUid = 0,
              .ownerGid = 0,
          },
          .sessionUid = lcl::security::kSessionUserUid,
          .sessionGid = lcl::security::kSessionUserGid,
      }),
      m_rasterService(platformServices) {}

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

bool Compositor::handleSystemGesture(
        SystemGestureDecision decision,
        const SystemGestureProgress& gesture) {
    const uint32_t windowId = m_windowManager.getFocusedWindowId();
    if (windowId == 0) return false;

    const auto surface = std::find_if(
        m_surfaces.begin(), m_surfaces.end(), [windowId](const auto& item) {
            return !item.second.isPopup() && !item.second.isAttached() &&
                item.second.systemSurfaceKind ==
                    protocol::LCLSystemSurfaceKind::None &&
                item.second.windowId == windowId;
        });
    if (surface == m_surfaces.end()) return false;

    auto& entry = surface->second;
    const auto phase = entry.transitionPhase;
    const bool visibilityExitInProgress =
        phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing ||
        phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
        entry.pendingDestroy || entry.pendingMinimize;
    if (visibilityExitInProgress) {
        // A second bottom-edge stream must not retarget an in-flight exit
        // spring back to Interactive/fullscreen. Only an explicit launcher
        // activation is allowed to restore a minimizing or closing surface.
        entry.launchGestureActive = false;
        return decision == SystemGestureDecision::Claim ||
               decision == SystemGestureDecision::Update ||
               decision == SystemGestureDecision::Cancel ||
               decision == SystemGestureDecision::Home;
    }

    if ((decision == SystemGestureDecision::Claim ||
         decision == SystemGestureDecision::Update) &&
        entry.hasLaunchOrigin) {
        if (entry.transitionPhase !=
            SurfaceRegistry::SurfaceEntry::TransitionPhase::Interactive) {
            entry.transitionElapsedSec = 0.0f;
        }
        entry.launchGestureActive = true;
        entry.launchGestureStartX = gesture.startX;
        entry.launchGestureStartY = gesture.startY;
        entry.launchGestureX = gesture.x;
        entry.launchGestureY = gesture.y;
        entry.launchGestureVelocityX = gesture.velocityX;
        entry.launchGestureVelocityY = gesture.velocityY;
        entry.launchGestureFlingPending = false;
        entry.transitionPhase =
            SurfaceRegistry::SurfaceEntry::TransitionPhase::Interactive;
        return true;
    }

    if (decision == SystemGestureDecision::Cancel) {
        if (!entry.launchGestureActive) return false;
        entry.launchGestureActive = false;
        entry.launchGestureVelocityX = 0.0f;
        entry.launchGestureVelocityY = 0.0f;
        entry.launchGestureFlingPending = false;
        entry.transitionElapsedSec = 0.0f;
        entry.transitionPhase =
            SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring;
        return true;
    }

    if (decision != SystemGestureDecision::Home) return false;

    // The scheduler replaces its gesture-following channel velocity with this
    // measured fling, so a quick release remains energetic after retargeting.
    entry.launchGestureActive = false;
    entry.launchGestureX = gesture.x;
    entry.launchGestureY = gesture.y;
    entry.launchGestureVelocityX = gesture.velocityX;
    entry.launchGestureVelocityY = gesture.velocityY;
    entry.launchGestureFlingPending = entry.hasLaunchOrigin;
    entry.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;
    entry.transitionElapsedSec = 0.0f;
    entry.transitionDurationSec = 0.18f;
    entry.transitionOpacity = 1.0f;
    entry.transitionScale = 1.0f;
    m_windowManager.transferFocusFromWindow(entry.windowId);
    return true;
}

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
    const auto& displayCorner =
        m_platformServices.gestalt().display.corners.topLeft;
    // Gestalt display geometry is expressed in physical pixels; compositor
    // transitions operate in logical units. WindowGroup currently has one
    // uniform corner style, and checked-in device profiles are symmetric.
    m_frameScheduler.setDisplayCornerStyle(
        std::min(displayCorner.radiusX, displayCorner.radiusY) / outputScale,
        displayCorner.roundness);
    m_windowingPolicy = makeWindowingPolicy(m_platformServices.gestalt().shell);
    m_protocolDispatcher = std::make_unique<ProtocolDispatcher>(
        m_renderer, m_windowManager, m_surfaces, m_sceneRegistry,
        m_focusController, m_shellStateBroker, *m_windowingPolicy,
        m_rasterService, m_peerAuthenticator);

    if (!m_rasterService.initialize(paths.rasterServiceExecutable(),
                                    paths.rasterSocketPath())) {
        std::cerr << "[LCL Core ERROR] retained raster service could not start\n";
        return false;
    }

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(paths.compositorSocketPath());

    // --- Route input through the dedicated focus/hit-test bridge ---
    m_inputRouter = std::make_unique<InputRouter>(
        m_windowManager, m_surfaces, m_sceneRegistry, outputScale,
        m_windowingPolicy->usesSystemGestures(),
        m_windowingPolicy->usesDesktopWindowManagement());
    m_inputRouter->setSystemGestureHandler(
        [this](SystemGestureDecision decision,
               const SystemGestureProgress& gesture) {
            return handleSystemGesture(decision, gesture);
        });
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
    m_rasterService.poll();
    if (m_protocolDispatcher) {
        for (auto& layer : m_rasterService.takeReadyLayers()) {
            (void)m_protocolDispatcher->acceptRasterLayer(std::move(layer));
        }
        // Storage import and snapshot promotion are intentionally separate.
        // A frame with any missing layer leaves the old presentation intact.
        for (auto& frame : m_rasterService.takePresentationFrames()) {
            if (m_protocolDispatcher->acceptPresentationFrame(std::move(frame))) {
                m_needsRedraw = true;
                m_shellStateDirty = true;
            }
        }
        // Android AHardwareBuffers are delivered on a side-band FIFO.  If a
        // manifest beat that FIFO by one poll, retry it after the import pass
        // without ever promoting a partial frame.
        if (m_protocolDispatcher->retryPendingPresentationFrames()) {
            m_needsRedraw = true;
            m_shellStateDirty = true;
        }
        for (auto& animation : m_rasterService.takePresentationAnimations()) {
            m_protocolDispatcher->acceptPresentationAnimation(
                std::move(animation));
        }
    }
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
    auto& display = m_platformServices.display();
    const auto& mode = display.activeMode();
    uint32_t refreshHz = (display.isInitialized() && mode.refreshRate > 0)
                         ? mode.refreshRate : 60;

    // 2. Calculate dynamic frame period & headroom allowance (~0.5ms safety budget)
    float targetPeriodMs = 1000.0f / static_cast<float>(refreshHz);
    float headroomMs = std::min(0.5f, targetPeriodMs * 0.08f);
    float targetBudgetMs = targetPeriodMs - headroomMs;
    auto targetFrameDuration = m_frameScheduler.frameBudgetForHz(refreshHz);

    std::cout << "[LCL Core] Dynamic Frame Pacer Active: " << refreshHz << " Hz "
              << "(Target Period: " << targetPeriodMs << " ms, Headroom: " << headroomMs
              << " ms, Budget: " << targetBudgetMs << " ms)\n";

    m_lastFpsTime = std::chrono::steady_clock::now();

    while (m_running.load()) {
        const auto& currentMode = display.activeMode();
        if (currentMode.refreshRate > 0 && currentMode.refreshRate != refreshHz) {
            refreshHz = currentMode.refreshRate;
            m_refreshIntervalNs = 1000000000ull / refreshHz;
            targetPeriodMs = 1000.0f / static_cast<float>(refreshHz);
            headroomMs = std::min(0.5f, targetPeriodMs * 0.08f);
            targetBudgetMs = targetPeriodMs - headroomMs;
            targetFrameDuration = m_frameScheduler.frameBudgetForHz(refreshHz);
            if (m_inputRouter) {
                m_inputRouter->setRefreshInterval(std::chrono::nanoseconds(m_refreshIntervalNs));
            }
            std::cout << "[LCL Core] Dynamic refresh rate switched to " << refreshHz << " Hz\n";
        }

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
        if (m_protocolDispatcher) {
            const auto animationNow = std::chrono::steady_clock::now();
            const float animationDelta = m_lastPresentationAnimationTick
                    .time_since_epoch().count() == 0
                ? 0.0f
                : std::chrono::duration<float>(
                      animationNow - m_lastPresentationAnimationTick).count();
            m_lastPresentationAnimationTick = animationNow;
            if (m_protocolDispatcher->advancePresentationAnimations(
                    animationDelta)) {
                m_needsRedraw = true;
            }
        }
        // Refresh-cadenced Live configures may have been deferred while an
        // input batch arrived. Revisit the coalesced newest geometry once per
        // compositor loop even when the pointer becomes stationary.
        if (m_inputRouter) m_inputRouter->syncWindowState();
        synchronizeShellState();
        bool willDraw = m_needsRedraw || m_windowManager.isAnyWindowDirty() || m_showFpsOverlay;

        if (willDraw) {
            // Hardware-backed displays provide the authoritative presentation
            // cadence. Waiting here keeps compositor spring frames from
            // free-running across an active scanout.
            const bool gotVsync = display.waitForVsync(
                std::chrono::nanoseconds(m_refreshIntervalNs * 2));
            renderFrame();

            // When hardware vSync cadence governs presentation, the hardware
            // vertical retrace edge already paced the frame. Avoid sleeping
            // unconditionally on software timers, which could overshoot the next
            // scanout and drop framerate.
            if (!gotVsync) {
                m_frameScheduler.waitForFrame(frameStart, targetFrameDuration);
            }
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
    // A successfully resolved launch can still lose its client before the
    // first raster layer. Never let that white fullscreen proxy permanently
    // cover Home/Wallpaper. Detach the visual after a bounded interval while
    // retaining a registered client surface so a late first frame can map as
    // an ordinary window.
    const auto launchNow = std::chrono::steady_clock::now();
    std::vector<SurfaceRegistry::Key> expiredLaunchPlaceholders;
    for (const auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.hasCommittedBuffer || entry.launchToken == 0 ||
            entry.launchPlaceholderDeadline.time_since_epoch().count() == 0 ||
            launchNow < entry.launchPlaceholderDeadline) {
            continue;
        }
        expiredLaunchPlaceholders.push_back(surfaceKey);
    }
    for (const auto surfaceKey : expiredLaunchPlaceholders) {
        auto found = m_surfaces.find(surfaceKey);
        if (found == m_surfaces.end()) continue;
        auto& entry = found->second;
        if (m_protocolDispatcher) {
            (void)m_protocolDispatcher->publishLaunchIconVisibility(
                entry, true);
        }
        if (entry.windowId != 0) {
            m_windowManager.removeWindow(entry.windowId);
            entry.windowId = 0;
        }
        if (entry.isLaunchPlaceholder) {
            m_surfaces.erase(found);
        } else {
            entry.launchToken = 0;
            entry.launchOwnerFd = -1;
            entry.hasLaunchOrigin = false;
            entry.launchMorphActive = false;
            entry.launchPlaceholderActive = false;
            entry.launchPlaceholderDeadline = {};
            entry.transitionPhase =
                SurfaceRegistry::SurfaceEntry::TransitionPhase::None;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
        }
        m_needsRedraw = true;
    }
    if (!m_needsRedraw && !m_windowManager.isAnyWindowDirty()) return;
    // Resolve ready WindowGroup epochs without stalling the output.
    // CompositorRenderer retains only an incomplete group's last complete
    // layer; the rest of the scene and all shell transitions keep presenting.
    (void)SurfaceTransactionCoordinator::promoteReady(
        m_surfaces, [this](uint32_t windowId, uint64_t generation) {
            return m_protocolDispatcher &&
                m_protocolDispatcher->commitAtomicSurfaceGeometry(
                    windowId, generation);
        });

    const bool hasActiveTransitions = m_frameScheduler.advanceTransitions(m_surfaces);
    const auto handoffNow = std::chrono::steady_clock::now();
    constexpr auto kLaunchIconHandoffTimeout = std::chrono::milliseconds(750);
    for (auto& [surfaceKey, entry] : m_surfaces) {
        (void)surfaceKey;
        if (entry.launchIconHandoffActive &&
            entry.launchIconHandoffDeadline.time_since_epoch().count() == 0) {
            entry.launchIconHandoffDeadline =
                handoffNow + kLaunchIconHandoffTimeout;
        }
        if (entry.launchIconRevealPending && m_protocolDispatcher &&
            m_protocolDispatcher->publishLaunchIconVisibility(entry, true)) {
            entry.launchIconRevealPending = false;
        }
        if (entry.launchIconHandoffActive &&
            handoffNow >= entry.launchIconHandoffDeadline) {
            // A dead or stalled HomeScreen must not leave the final proxy or
            // closing surface alive indefinitely.
            entry.launchIconRevealPending = false;
            entry.launchIconHandoffActive = false;
            entry.launchIconHandoffDeadline = {};
            entry.launchMorphActive = false;
        }
    }
    for (auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.pendingMinimize && !entry.launchIconHandoffActive) {
            m_windowManager.minimizeWindow(entry.windowId);
            entry.pendingMinimize = false;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
        }
    }
    const auto surfaces = m_surfaces.snapshot();
    const auto composeStart = std::chrono::steady_clock::now();
    const uint64_t composeStartNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            composeStart.time_since_epoch()).count());
    m_compositorRenderer.render(
        m_renderer, m_platformServices.display(), m_windowManager, surfaces,
        [this] { renderDiagnosticOverlay(); },
        !hasActiveTransitions && !m_showFpsOverlay,
        m_windowingPolicy &&
            m_windowingPolicy->usesMobileWindowDecorations(),
        hasActiveTransitions);
    m_lastComposeMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - composeStart).count();

    // Old raster layers are released only after the frame that stopped
    // referencing them has been handed to the platform presenter. A pending
    // atomic group still presents its retained old snapshot, so its sources
    // remain owned until that group is promoted and composed.
    for (auto& [surfaceKey, entry] : m_surfaces) {
        (void)surfaceKey;
        if (entry.atomicConfigureGeneration != 0) continue;
        for (const auto& release : entry.pendingRasterLayerReleases) {
            const int releaseFenceFd = release.texture != 0
                ? m_renderer.createNativeFence() : -1;
            if (release.texture != 0) {
                m_renderer.getRasterRenderer()->releaseDmaBufTexture(
                    release.texture);
            }
            m_rasterService.releaseLayer(
                release.layerId,
                raster_protocol::LayerReleaseReason::Presented,
                releaseFenceFd);
        }
        entry.pendingRasterLayerReleases.clear();
    }

    const uint64_t displaySequence = ++m_displaySequence;
    const uint64_t presentedAtNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.clientFd < 0 || !entry.hasCommittedBuffer ||
            entry.atomicConfigureGeneration != 0 ||
            !SurfaceRegistry::hasUnpresentedFrame(entry)) continue;
        protocol::LCLHeader header{};
        header.opcode = protocol::LCLOpcode::FramePresented;
        header.payloadSize = sizeof(protocol::LCLMsgFramePresented);
        protocol::LCLMsgFramePresented message{};
        message.surfaceId = static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu);
        message.configureSerial = entry.presentationSerial;
        message.frameSerial = entry.frameSerial != 0
            ? entry.frameSerial : entry.presentationSerial;
        message.geometryGeneration = entry.layerGeometryGeneration;
        message.displaySequence = displaySequence;
        message.timestampNs = presentedAtNs;
        message.refreshIntervalNs = m_refreshIntervalNs;
        message.clientFrameStartNs = entry.clientFrameStartNs;
        message.clientSubmitNs = entry.clientSubmitNs;
        message.rasterStartNs = entry.rasterStartNs;
        message.rasterReadyNs = entry.rasterReadyNs;
        message.composeStartNs = entry.clientFrameStartNs != 0
            ? composeStartNs : 0;
        if (protocol::sendMsgWithFd(entry.clientFd, header, &message)) {
            SurfaceRegistry::completePresentation(entry);
        }
    }

    std::vector<SurfaceRegistry::Key> surfacesToRemove;
    for (const auto& [surfaceKey, entry] : m_surfaces) {
        if (entry.pendingDestroy && !entry.launchIconHandoffActive) {
            surfacesToRemove.push_back(surfaceKey);
            if (!entry.isAttached() && entry.windowId != 0) {
                const auto attachments =
                    m_surfaces.attachedChildren(entry.windowId);
                surfacesToRemove.insert(surfacesToRemove.end(),
                                        attachments.begin(), attachments.end());
            }
        }
    }
    std::sort(surfacesToRemove.begin(), surfacesToRemove.end());
    surfacesToRemove.erase(
        std::unique(surfacesToRemove.begin(), surfacesToRemove.end()),
        surfacesToRemove.end());
    for (const auto surfaceKey : surfacesToRemove) {
        const auto found = m_surfaces.find(surfaceKey);
        if (found != m_surfaces.end() && !found->second.isAttached() &&
            found->second.windowId > 0) {
            m_windowManager.removeWindow(found->second.windowId);
        }
        if (found != m_surfaces.end()) {
            if (found->second.producerGrant.tokenHigh != 0 ||
                found->second.producerGrant.tokenLow != 0) {
                if (m_protocolDispatcher) {
                    m_protocolDispatcher->releaseSurfaceRasterState(
                        found->second);
                } else {
                    m_rasterService.revokeSurface(found->second.producerGrant);
                }
            }
            if (found->second.rasterLayerId != 0) {
                const int releaseFenceFd =
                    found->second.rasterLayerTexture != 0
                    ? m_renderer.createNativeFence() : -1;
                if (found->second.rasterLayerTexture != 0) {
                    m_renderer.getRasterRenderer()->releaseDmaBufTexture(
                        found->second.rasterLayerTexture);
                    found->second.rasterLayerTexture = 0;
                }
                m_rasterService.releaseLayer(
                    found->second.rasterLayerId,
                    raster_protocol::LayerReleaseReason::Presented,
                    releaseFenceFd);
                found->second.rasterLayerId = 0;
            }
            for (const auto& release :
                 found->second.pendingRasterLayerReleases) {
                const int releaseFenceFd = release.texture != 0
                    ? m_renderer.createNativeFence() : -1;
                if (release.texture != 0) {
                    m_renderer.getRasterRenderer()->releaseDmaBufTexture(
                        release.texture);
                }
                m_rasterService.releaseLayer(
                    release.layerId,
                    raster_protocol::LayerReleaseReason::Presented,
                    releaseFenceFd);
            }
            found->second.pendingRasterLayerReleases.clear();
            if (found->second.isAttached() &&
                !found->second.pendingDestroy &&
                found->second.clientFd >= 0) {
                protocol::LCLHeader header{};
                header.opcode = protocol::LCLOpcode::SurfaceDestroy;
                header.payloadSize = sizeof(protocol::LCLMsgSurfaceDestroy);
                protocol::LCLMsgSurfaceDestroy destroy{};
                destroy.surfaceId = static_cast<uint32_t>(
                    surfaceKey & 0xFFFFFFFFu);
                protocol::sendMsgWithFd(
                    found->second.clientFd, header, &destroy);
            }
            SurfaceRegistry::releaseBuffer(found->second);
        }
        m_sceneRegistry.removeSurface(surfaceKey);
        m_surfaces.erase(surfaceKey);
        m_shellStateDirty = true;
    }

    synchronizeShellState();

    m_windowManager.clearAllDirty();
    const bool hasActiveLaunchIconHandoff = std::any_of(
        m_surfaces.begin(), m_surfaces.end(), [](const auto& pair) {
            return pair.second.launchIconHandoffActive;
        });
    m_needsRedraw = hasActiveTransitions || !surfacesToRemove.empty() ||
                    hasActiveLaunchIconHandoff;
    ++m_fpsFrameCount;
}

} // namespace lcl::core
