#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/compositor/surface_registry.hpp"
#include "core/compositor/input_router.hpp"
#include "core/compositor/frame_scheduler.hpp"
#include "core/compositor/protocol_dispatcher.hpp"
#include "core/compositor/compositor_renderer.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/**
 * @brief LCL Compositor — Pure Display Server & Compositor orchestrator.
 *
 * Responsibilities:
 *  - Subsystem initialization and ordered shutdown
 *  - IPC message and protocol packet dispatch
 *  - Direct DRM/KMS and EGL hardware-accelerated frame composition
 *  - Frame pacing at target display refresh rate
 */
class Compositor {
public:
    Compositor();
    ~Compositor();

    // Non-copyable, non-moveable
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    /**
     * @brief Initialize all subsystems in dependency order.
     * @return true if core subsystems came up, false on fatal error.
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

private:
    void processInput();
    void processIPC();
    void renderFrame();
    void renderDiagnosticOverlay();
    void publishWindowListToShellClients();

    // Subsystems — declared in init order, destructed in reverse
    DisplayManager         m_displayManager;
    InputManager           m_inputManager;
    IPCManager             m_ipcManager;
    render::Renderer       m_renderer;
    render::WindowManager  m_windowManager;

    /// IPC surface registry: (clientFd << 32 | surfaceId) → SurfaceEntry
    SurfaceRegistry m_surfaces;
    std::unique_ptr<InputRouter> m_inputRouter;
    FrameScheduler m_frameScheduler;
    CompositorRenderer m_compositorRenderer;
    std::unique_ptr<ProtocolDispatcher> m_protocolDispatcher;

    // Loop state
    std::atomic<bool>                    m_running{true};
    bool                                 m_initialized{false};
    bool                                 m_needsRedraw{true};
    uint64_t                             m_loopTicks{0};

    // Diagnostic Overlay & FPS metrics
    bool                                 m_showFpsOverlay{false};
    uint32_t                             m_fpsFrameCount{0};
    float                                m_currentFps{0.0f};
    float                                m_currentFrameMs{0.0f};
    std::chrono::steady_clock::time_point m_lastFpsTime;
};

} // namespace lcl::core
