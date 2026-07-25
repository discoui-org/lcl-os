#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "core/input/input_manager.hpp"

namespace lcl::render {

struct Window {
    uint32_t id{0};
    std::string title;
    int x{0};
    int y{0};
    int width{400};
    int height{300};
    int zIndex{0};
    bool isFocused{false};
    bool isDragging{false};
    int dragOffsetX{0};
    int dragOffsetY{0};
    uint32_t headerColor{0xFF38BDF8};
};

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    // Non-copyable
    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // Moveable
    WindowManager(WindowManager&&) noexcept;
    WindowManager& operator=(WindowManager&&) noexcept;

    /**
     * @brief Initialize window manager canvas dimensions.
     */
    bool initialize(uint32_t screenWidth = 1024, uint32_t screenHeight = 768);

    /**
     * @brief Create a new window dynamically.
     */
    uint32_t createWindow(const std::string& title, int x, int y, int width, int height, uint32_t headerColor = 0xFF38BDF8);

    /**
     * @brief Remove / close a window by ID.
     */
    bool removeWindow(uint32_t windowId);

    /**
     * @brief Process input event for hit testing, window focus, and dragging.
     */
    void processInputEvent(const core::InputEvent& ev);

    /**
     * @brief Focus a window by ID and bring it to top z-order.
     */
    void focusWindow(uint32_t windowId);

    const std::vector<Window>& getWindows() const { return m_windows; }
    int getMouseX() const { return m_mouseX; }
    int getMouseY() const { return m_mouseY; }

private:
    void updateWindowZOrders();

    uint32_t m_screenWidth{1024};
    uint32_t m_screenHeight{768};
    std::vector<Window> m_windows;
    int m_mouseX{512};
    int m_mouseY{384};
    uint32_t m_nextWindowId{1};
    bool m_initialized{false};
};

} // namespace lcl::render
