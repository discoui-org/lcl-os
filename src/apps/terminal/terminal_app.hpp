#pragma once

#include <string>
#include <vector>
#include "core/terminal/pty_manager.hpp"
#include "core/input/input_manager.hpp"

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
     */
    void update();

    /**
     * @brief Handle keyboard input event for active terminal shell.
     */
    void handleInput(const core::InputEvent& ev);

    /**
     * @brief Clear terminal line buffer (Ctrl+L / clear ANSI escape sequence).
     */
    void clearBuffer();

    /**
     * @brief Shut down Terminal PTY process and cleanup.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getWindowId() const { return m_windowId; }
    const std::vector<std::string>& getLines() const { return m_lines; }

    bool isAlive() const { return m_initialized && m_ptyManager.isAlive(); }

    void setAckFifo(const std::string& fifo) { m_ackFifo = fifo; }
    const std::string& getAckFifo() const { return m_ackFifo; }

private:
    std::string keycodeToASCII(uint32_t keycode, bool shift);
    std::string stripANSI(const std::string& input);

    int m_windowId{-1};
    core::PTYManager m_ptyManager;
    std::vector<std::string> m_lines;
    std::string m_currentLine;
    std::string m_ackFifo;
    bool m_initialized{false};
    bool m_shiftPressed{false};
    bool m_ctrlPressed{false};
};

} // namespace lcl::apps
