#include "render/window_manager.hpp"
#include <iostream>
#include <algorithm>
#include <linux/input-event-codes.h>

namespace lcl::render {

WindowManager::WindowManager() = default;
WindowManager::~WindowManager() = default;

void WindowManager::initialize(int screenWidth, int screenHeight) {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_mouseX = screenWidth / 2;
    m_mouseY = screenHeight / 2;

    m_windows.clear();

    // Primary demo window: LCL Terminal
    Window win1;
    win1.id = 1;
    win1.title = "LCL Terminal / Core Engine";
    win1.x = 80;
    win1.y = 80;
    win1.width = 540;
    win1.height = 360;
    win1.headerColor = 0xFF89B4FA; // Active blue
    win1.isFocused = true;
    m_windows.push_back(win1);

    // Secondary demo window: LCL System Monitor
    Window win2;
    win2.id = 2;
    win2.title = "LCL System Monitor";
    win2.x = 360;
    win2.y = 200;
    win2.width = 460;
    win2.height = 300;
    win2.headerColor = 0xFF45475A; // Unfocused grey
    win2.isFocused = false;
    m_windows.push_back(win2);

    std::cout << "[LCL WindowManager] Initialized with " << m_windows.size() << " active spatial windows.\n";
}

void WindowManager::updateScreenSize(int width, int height) {
    m_screenWidth = width;
    m_screenHeight = height;
}

void WindowManager::processInputEvent(const core::InputEvent& event) {
    if (event.type == core::InputEventType::PointerMotion) {
        if (event.absoluteX >= 0.0 && event.absoluteY >= 0.0) {
            m_mouseX = std::clamp(static_cast<int>(event.absoluteX), 0, m_screenWidth - 1);
            m_mouseY = std::clamp(static_cast<int>(event.absoluteY), 0, m_screenHeight - 1);
        } else {
            m_mouseX = std::clamp(m_mouseX + static_cast<int>(event.dx), 0, m_screenWidth - 1);
            m_mouseY = std::clamp(m_mouseY + static_cast<int>(event.dy), 0, m_screenHeight - 1);
        }

        // If dragging active window, move it smoothly
        for (auto& win : m_windows) {
            if (win.isDragging) {
                win.x = std::clamp(m_mouseX - win.dragOffsetX, 0, m_screenWidth - win.width);
                win.y = std::clamp(m_mouseY - win.dragOffsetY, 40, m_screenHeight - win.height);
            }
        }
    } else if (event.type == core::InputEventType::PointerButton) {
        if (event.button == BTN_LEFT) {
            m_mouseLeftDown = event.pressed;

            if (m_mouseLeftDown) {
                // Check windows in top-to-bottom z-order (reverse iteration)
                bool hitHandled = false;
                for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                    auto& win = m_windows[i];

                    // Check titlebar hit test (y .. y + 32)
                    if (m_mouseX >= win.x && m_mouseX < win.x + win.width &&
                        m_mouseY >= win.y && m_mouseY < win.y + 32) {

                        // Bring window to front
                        bringToFront(i);

                        // Start dragging
                        m_windows.back().isDragging = true;
                        m_windows.back().dragOffsetX = m_mouseX - m_windows.back().x;
                        m_windows.back().dragOffsetY = m_mouseY - m_windows.back().y;
                        hitHandled = true;
                        break;
                    }
                    // Check general window body hit test
                    else if (m_mouseX >= win.x && m_mouseX < win.x + win.width &&
                             m_mouseY >= win.y && m_mouseY < win.y + win.height) {
                        bringToFront(i);
                        hitHandled = true;
                        break;
                    }
                }
                (void)hitHandled;
            } else {
                // Button released - stop all window drag states
                for (auto& win : m_windows) {
                    win.isDragging = false;
                }
            }
        }
    }
}

void WindowManager::bringToFront(size_t index) {
    if (index >= m_windows.size()) return;

    Window targetWin = m_windows[index];
    m_windows.erase(m_windows.begin() + index);

    // Update focus states
    for (auto& win : m_windows) {
        win.isFocused = false;
        win.headerColor = 0xFF45475A;
    }

    targetWin.isFocused = true;
    targetWin.headerColor = 0xFF89B4FA; // Active highlight
    m_windows.push_back(targetWin);
}

} // namespace lcl::render
