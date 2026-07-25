#pragma once

#include <string>
#include <vector>
#include <memory>
#include "core/terminal/pty_manager.hpp"
#include "core/input/input_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::apps {

class TerminalApp {
public:
    TerminalApp();
    ~TerminalApp();

    // Non-copyable
    TerminalApp(const TerminalApp&) = delete;
    TerminalApp& operator=(const TerminalApp&) = delete;

    /**
     * @brief Initialize modular Terminal application and spawn PTY shell.
     * @param windowId Window Manager target window ID
     * @return true if initialized, false otherwise
     */
    bool initialize(int windowId = 1);

    /**
     * @brief Process input event passed from compositor/window manager.
     */
    void handleInput(const core::InputEvent& ev);

    /**
     * @brief Poll PTY output and update internal line buffers.
     */
    void update();

    /**
     * @brief Clear terminal screen buffer (Ctrl+L / clear command).
     */
    void clearBuffer();

    /**
     * @brief Terminate PTY shell process and cleanup resources.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getWindowId() const { return m_windowId; }
    const std::vector<std::string>& getLines() const { return m_lines; }

private:
    std::string stripANSI(const std::string& input);
    std::string keycodeToASCII(uint32_t keycode, bool shift);

    int m_windowId{1};
    core::PTYManager m_ptyManager;
    std::vector<std::string> m_lines;
    std::string m_currentLine;
    bool m_shiftPressed{false};
    bool m_ctrlPressed{false};
    bool m_initialized{false};
};

} // namespace lcl::apps
