#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "platforms/common/platform_services.hpp"
#include "system/ipc/ipc_manager.hpp"
#include "system/compositor/surface_registry.hpp"
#include "system/compositor/input_router.hpp"
#include "system/compositor/frame_scheduler.hpp"
#include "system/compositor/protocol_dispatcher.hpp"
#include "system/compositor/windowing_policy.hpp"
#include "system/compositor/compositor_renderer.hpp"
#include "system/compositor/raster_service_host.hpp"
#include "system/scene/focus_controller.hpp"
#include "system/scene/scene_registry.hpp"
#include "system/scene/shell_state_broker.hpp"
#include "system/render/renderer.hpp"
#include "system/render/window_manager.hpp"
#include "system/security/application_peer_authenticator.hpp"

namespace lcl::core {

/**
 * @brief LCL Compositor — Pure Display Server & Compositor orchestrator.
 *
 * Consumes platform-agnostic IPlatformServices injected via composition root.
 * Completely free of any platform-specific DRM, GBM, EGL, or evdev types.
 */
class Compositor {
public:
    explicit Compositor(lcl::platform::IPlatformServices& platformServices);
    ~Compositor();

    // Non-copyable, non-moveable
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    /**
     * @brief Initialize all core compositor subsystems.
     * @return true if core subsystems initialized successfully, false on fatal error.
     */
    bool initialize();

    /**
     * @brief Run the main compositor event/render loop.
     * Blocks until requestShutdown() is called (typically via signal handler).
     */
    void run();

    /**
     * @brief Thread-safe shutdown request. Safe to call from signal handlers.
     */
    void requestShutdown() noexcept { m_running.store(false); }

    bool isRunning() const noexcept { return m_running.load(); }

    using SurfaceEffectRegion = SurfaceRegistry::SurfaceEffectRegion;
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;

    void toggleFpsOverlay() noexcept { m_showFpsOverlay = !m_showFpsOverlay; }
    bool isFpsOverlayVisible() const noexcept { return m_showFpsOverlay; }
    size_t activeSurfaceCount() const noexcept { return m_surfaces.snapshot().size(); }
    size_t activeWindowCount() const noexcept { return m_windowManager.getWindows().size(); }

private:
    void processInput();
    void processIPC();
    void renderFrame();
    void renderDiagnosticOverlay();
    void synchronizeShellState();
    bool handleSystemGesture(SystemGestureDecision decision,
                             const SystemGestureProgress& gesture);

    // Injected Platform Services reference
    lcl::platform::IPlatformServices& m_platformServices;

    // Subsystems — declared in init order, destructed in reverse
    lcl::security::ApplicationPeerAuthenticator m_peerAuthenticator;
    IPCManager             m_ipcManager;
    render::Renderer       m_renderer;
    render::WindowManager  m_windowManager;

    /// IPC surface registry: (clientFd << 32 | surfaceId) → SurfaceEntry
    SurfaceRegistry m_surfaces;
    RasterServiceHost m_rasterService;
    SceneRegistry m_sceneRegistry;
    FocusController m_focusController;
    ShellStateBroker m_shellStateBroker;
    std::unique_ptr<WindowingPolicy> m_windowingPolicy;
    std::unique_ptr<InputRouter> m_inputRouter;
    FrameScheduler m_frameScheduler;
    CompositorRenderer m_compositorRenderer;
    std::unique_ptr<ProtocolDispatcher> m_protocolDispatcher;

    // Loop state
    std::atomic<bool>                    m_running{true};
    bool                                 m_initialized{false};
    bool                                 m_needsRedraw{true};
    bool                                 m_shellStateDirty{true};
    uint64_t                             m_loopTicks{0};
    uint64_t                             m_refreshIntervalNs{16666667};
    std::chrono::steady_clock::time_point m_lastPresentationAnimationTick{};
    uint64_t                             m_displaySequence{0};

    // Diagnostic Overlay & FPS metrics
    bool                                 m_showFpsOverlay{false};
    uint32_t                             m_fpsFrameCount{0};
    float                                m_currentFps{0.0f};
    float                                m_currentFrameMs{0.0f};
    float                                m_lastComposeMs{0.0f};
    std::chrono::steady_clock::time_point m_lastFpsTime;
};

} // namespace lcl::core
