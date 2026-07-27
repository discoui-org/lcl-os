#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "core/terminal/pty_manager.hpp"
#include "core/input/input_manager.hpp"
#include "render/render_types.hpp"

namespace lcl::apps {

class TerminalApp {
public:
    TerminalApp();
    ~TerminalApp();

    // Non-copyable
    TerminalApp(const TerminalApp&) = delete;
    TerminalApp& operator=(const TerminalApp&) = delete;

    /**
     * @brief Initialize Terminal application instance with spatial window ID.
     */
    bool initialize(int windowId);

    /**
     * @brief Poll PTY output buffer and update terminal line rendering state.
     * @return True if new output was read or state changed.
     */
    bool update();

    /**
     * @brief Handle keyboard input event for active terminal shell.
     * Pure PTY passthrough — all line editing, history, and tab completion
     * is delegated to the shell (bash/readline) inside the PTY.
     */
    void handleInput(const core::InputEvent& ev);
    void handleKey(uint32_t keycode, bool pressed);

    /**
     * @brief Clear terminal line buffer (Ctrl+L / clear ANSI escape sequence).
     */
    void clearBuffer();
    void resize(int width, int height);

    /**
     * @brief Shut down Terminal PTY process and cleanup.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getWindowId() const { return m_windowId; }
    const std::vector<std::string>& getLines() const { return m_lines; }

    bool isAlive() const { return m_initialized && m_ptyManager.isAlive(); }

    /**
     * @brief Encapsulate active terminal rendering state for Compositor WindowManager.
     */
    render::WindowRenderContent getRenderContent() const;

    void setAckClientFd(int fd) { m_ackClientFd = fd; }
    int  getAckClientFd() const  { return m_ackClientFd; }

private:
    std::string keycodeToASCII(uint32_t keycode, bool shift);

    int m_windowId{-1};
    core::PTYManager m_ptyManager;
    std::vector<std::string> m_lines;
    int m_writePos{0}; // Write-head byte offset in m_lines.back() (VT100 overwrite tracking)
    std::chrono::steady_clock::time_point m_lastInputTime;
    int m_ackClientFd{-1}; ///< Client socket FD awaiting DONE ACK (-1 = none)
    bool m_initialized{false};
    bool m_shiftPressed{false};
    bool m_ctrlPressed{false};
};

} // namespace lcl::apps
