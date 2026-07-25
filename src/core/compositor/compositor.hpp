#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "core/display/display_manager.hpp"
#include "core/input/input_manager.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/session/startup_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"
#include "apps/terminal/terminal_app.hpp"

namespace lcl::core {

/**
 * @brief LCL Compositor — Top-level orchestrator owning all subsystems and
 *        driving the main frame/event loop.
 *
 * Responsibilities:
 *  - Subsystem initialization and ordered shutdown
 *  - IPC message dispatch (SPAWN_TERMINAL, DESTROY_LAST_WINDOW)
 *  - Window ↔ App lifecycle synchronization
 *  - PTY process health tracking
 *  - Frame pacing at target display refresh rate
 *
 * Usage:
 *  @code
 *  Compositor c;
 *  if (c.initialize()) c.run();
 *  @endcode
 */
class Compositor {
public:
    Compositor();
    ~Compositor();

    // Non-copyable, non-moveable (owns subsystems by value)
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

private:
    // ----------------------------------------------------------------
    // Per-tick processing — called once per frame in run()
    // ----------------------------------------------------------------
    void processInput();
    void processIPC();
    void syncWindowLifecycle(); ///< Remove apps whose UI close button was clicked
    void updateApps();          ///< Poll PTY output; clean up exited processes
    void tickCursorBlink();
    void renderFrame();

    // ----------------------------------------------------------------
    // IPC command handlers
    // ----------------------------------------------------------------
    void handleSpawnTerminal(const IPCClientMessage& msg, bool waitMode);
    void handleDestroyLastWindow();

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------
    /// Send "DONE" ACK over client socket fd; no-op if fd < 0.
    void sendAck(int fd);

    // ----------------------------------------------------------------
    // Subsystems — declared in init order, destructed in reverse
    // ----------------------------------------------------------------
    DisplayManager         m_displayManager;
    InputManager           m_inputManager;
    IPCManager             m_ipcManager;
    StartupManager         m_startupManager;
    render::Renderer       m_renderer;
    render::WindowManager  m_windowManager;

    // ----------------------------------------------------------------
    // Application registry
    // ----------------------------------------------------------------
    std::vector<std::unique_ptr<apps::TerminalApp>> m_apps;

    // ----------------------------------------------------------------
    // Loop state
    // ----------------------------------------------------------------
    std::atomic<bool>                    m_running{true};
    bool                                 m_initialized{false};
    bool                                 m_needsRedraw{true};
    uint64_t                             m_loopTicks{0};
    std::chrono::microseconds            m_targetFrameDuration{std::chrono::microseconds(16667)};
    std::chrono::steady_clock::time_point m_lastBlinkCheck;
};

} // namespace lcl::core
