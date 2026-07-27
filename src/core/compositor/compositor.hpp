#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/session/startup_manager.hpp"
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

    /// Per-IPC-surface metadata tracked by the compositor.
    struct SurfaceEntry {
        uint32_t windowId{0};    ///< Corresponding WindowManager window id
        int      clientFd{-1};   ///< Socket FD of client process
        int      shmFd{-1};     ///< memfd descriptor received via SCM_RIGHTS
        void*    pixels{nullptr};///< mmap'd pixel pointer into the SHM buffer
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};     ///< Row stride in bytes
        size_t   shmSize{0};    ///< Total SHM buffer bytes
    };

private:
    void processInput();
    void processIPC();
    void tickCursorBlink();
    void renderFrame();

    // Subsystems — declared in init order, destructed in reverse
    DisplayManager         m_displayManager;
    InputManager           m_inputManager;
    IPCManager             m_ipcManager;
    StartupManager         m_startupManager;
    render::Renderer       m_renderer;
    render::WindowManager  m_windowManager;

    /// IPC surface registry: (clientFd << 32 | surfaceId) → SurfaceEntry
    std::unordered_map<uint64_t, SurfaceEntry> m_surfaces;

    // Loop state
    std::atomic<bool>                    m_running{true};
    bool                                 m_initialized{false};
    bool                                 m_needsRedraw{true};
    uint64_t                             m_loopTicks{0};
    std::chrono::microseconds            m_targetFrameDuration{std::chrono::microseconds(16667)};
    std::chrono::steady_clock::time_point m_lastBlinkCheck;
};

} // namespace lcl::core
