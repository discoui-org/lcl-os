#include "render/window_manager.hpp"
#include <iostream>
#include <algorithm>
#include <linux/input-event-codes.h>

namespace lcl::render {

WindowManager::WindowManager() = default;
WindowManager::~WindowManager() = default;

bool WindowManager::initialize(uint32_t screenWidth, uint32_t screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_mouseX = screenWidth / 2;
    m_mouseY = screenHeight / 2;
    m_windows.clear();
    m_initialized = true;

    std::cout << "[LCL WindowManager] Initialized canvas (" << m_screenWidth << "x" << m_screenHeight << ") with 0 dummy windows.\n";
    return true;
}

uint32_t WindowManager::createWindow(const std::string& title, int x, int y, int width, int height, uint32_t headerColor) {
    for (auto& w : m_windows) {
        w.isFocused = false;
        w.headerColor = 0xFF45475A;
    }

    Window win{};
    win.id = m_nextWindowId++;
    win.title = title;
    win.x = x;
    win.y = y;
    win.width = width;
    win.height = height;
    win.headerColor = headerColor;
    win.isFocused = true;

    m_windows.push_back(win);
    std::cout << "[LCL WindowManager] Created window ID: " << win.id << " ('" << title << "') at (" << x << "," << y << ").\n";
    return win.id;
}

void WindowManager::processInputEvent(const core::InputEvent& event) {
    if (event.type == core::InputEventType::PointerMotion) {
        if (event.absoluteX >= 0.0 && event.absoluteY >= 0.0) {
            m_mouseX = std::clamp(static_cast<int>(event.absoluteX), 0, static_cast<int>(m_screenWidth) - 1);
            m_mouseY = std::clamp(static_cast<int>(event.absoluteY), 0, static_cast<int>(m_screenHeight) - 1);
        } else {
            m_mouseX = std::clamp(m_mouseX + static_cast<int>(event.dx), 0, static_cast<int>(m_screenWidth) - 1);
            m_mouseY = std::clamp(m_mouseY + static_cast<int>(event.dy), 0, static_cast<int>(m_screenHeight) - 1);
        }

        // Move dragging window
        for (auto& win : m_windows) {
            if (win.isDragging) {
                win.x = std::clamp(m_mouseX - win.dragOffsetX, 0, static_cast<int>(m_screenWidth) - win.width);
                win.y = std::clamp(m_mouseY - win.dragOffsetY, 40, static_cast<int>(m_screenHeight) - win.height);
            }
        }
    } else if (event.type == core::InputEventType::PointerButton) {
        if (event.button == BTN_LEFT) {
            if (event.pressed) {
                for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                    auto& win = m_windows[i];
                    if (m_mouseX >= win.x && m_mouseX < win.x + win.width &&
                        m_mouseY >= win.y && m_mouseY < win.y + win.height) {
                        focusWindow(win.id);
                        win.isDragging = (m_mouseY < win.y + 32);
                        if (win.isDragging) {
                            win.dragOffsetX = m_mouseX - win.x;
                            win.dragOffsetY = m_mouseY - win.y;
                        }
                        break;
                    }
                }
            } else {
                for (auto& win : m_windows) {
                    win.isDragging = false;
                }
            }
        }
    }
}

void WindowManager::focusWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });

    if (it != m_windows.end()) {
        Window target = *it;
        m_windows.erase(it);

        for (auto& w : m_windows) {
            w.isFocused = false;
            w.headerColor = 0xFF45475A;
        }

        target.isFocused = true;
        target.headerColor = 0xFF89B4FA;
        m_windows.push_back(target);
    }
}

} // namespace lcl::render
