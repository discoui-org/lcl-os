#pragma once

#include <string>
#include <vector>
#include <memory>
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
    uint32_t headerColor{0xFF313244};
    bool isFocused{false};
    bool isDragging{false};
    int dragOffsetX{0};
    int dragOffsetY{0};
};

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    void initialize(int screenWidth, int screenHeight);
    void updateScreenSize(int width, int height);

    /**
     * @brief Process an input event for mouse movement, window click, and dragging.
     */
    void processInputEvent(const core::InputEvent& event);

    const std::vector<Window>& getWindows() const { return m_windows; }
    int getMouseX() const { return m_mouseX; }
    int getMouseY() const { return m_mouseY; }

private:
    void bringToFront(size_t index);

    int m_screenWidth{1024};
    int m_screenHeight{768};
    int m_mouseX{512};
    int m_mouseY{384};
    bool m_mouseLeftDown{false};
    std::vector<Window> m_windows;
};

} // namespace lcl::render
