#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "core/terminal/pty_manager.hpp"
#include "platform/common/input_backend.hpp"
#include "platform/common/keyboard_mapper.hpp"

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
    void handleInput(const lcl::platform::RawInputEvent& ev);
    void handleKey(lcl::platform::PhysicalKey key, bool pressed, uint8_t modifiers = 0, char32_t codepoint = 0);
    void handleKey(uint32_t keycode, bool pressed, uint8_t modifiers = 0, char32_t codepoint = 0);

    /** Write UTF-8 text received from WindowApp's text-input lifecycle. */
    void handleText(const std::string& text);

    /**
     * @brief Clear terminal line buffer (Ctrl+L / clear ANSI escape sequence).
     */
    void clearBuffer();
    void resize(int width, int height);
    static void getSnappedDimensions(int reqW, int reqH, int& outW, int& outH);

    /**
     * @brief Shut down Terminal PTY process and cleanup.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getWindowId() const { return m_windowId; }
    const std::vector<std::string>& getLines() const { return m_lines; }
    int getCursorColumn() const { return m_writePos; }
    bool shouldDrawSolidCursor() const;

    bool isAlive() const { return m_initialized && m_ptyManager.isAlive(); }

private:
    int m_windowId{-1};
    core::PTYManager m_ptyManager;
    std::vector<std::string> m_lines;
    int m_writePos{0}; // Write-head byte offset in m_lines.back() (VT100 overwrite tracking)
    std::chrono::steady_clock::time_point m_lastInputTime;
    bool m_initialized{false};
};

} // namespace lcl::apps
