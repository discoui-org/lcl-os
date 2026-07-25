#include "render/window_manager.hpp"
#include "core/display/display_scale.hpp"
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
        w.markDirty();
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
    win.markDirty();

    m_windows.push_back(win);
    m_mouseDirty = true;
    std::cout << "[LCL WindowManager] Created window ID: " << win.id << " ('" << title << "') at (" << x << "," << y << ").\n";
    return win.id;
}

bool WindowManager::removeWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });

    if (it != m_windows.end()) {
        std::cout << "[LCL WindowManager] Removing window ID: " << windowId << " ('" << it->title << "').\n";
        m_windows.erase(it);

        if (!m_windows.empty()) {
            m_windows.back().isFocused = true;
            m_windows.back().headerColor = 0xFF89B4FA;
        }
        markAllDirty();
        return true;
    }
    return false;
}

bool WindowManager::processInputEvent(const core::InputEvent& event) {
    bool stateChanged = false;

    if (event.type == core::InputEventType::PointerMotion) {
        int oldX = m_mouseX;
        int oldY = m_mouseY;

        if (event.absoluteX >= 0.0 && event.absoluteY >= 0.0) {
            m_mouseX = std::clamp(static_cast<int>(event.absoluteX), 0, static_cast<int>(m_screenWidth) - 1);
            m_mouseY = std::clamp(static_cast<int>(event.absoluteY), 0, static_cast<int>(m_screenHeight) - 1);
        } else {
            m_mouseX = std::clamp(m_mouseX + static_cast<int>(event.dx), 0, static_cast<int>(m_screenWidth) - 1);
            m_mouseY = std::clamp(m_mouseY + static_cast<int>(event.dy), 0, static_cast<int>(m_screenHeight) - 1);
        }

        if (m_mouseX != oldX || m_mouseY != oldY) {
            m_mouseDirty = true;
            stateChanged = true;
        }

        // Move dragging window
        for (auto& win : m_windows) {
            if (win.isDragging) {
                const int menuH = core::DisplayScale::menuBarHeight();
                int newX = std::clamp(m_mouseX - win.dragOffsetX, 0, static_cast<int>(m_screenWidth) - win.width);
                int newY = std::clamp(m_mouseY - win.dragOffsetY, menuH, static_cast<int>(m_screenHeight) - win.height);
                if (newX != win.x || newY != win.y) {
                    win.x = newX;
                    win.y = newY;
                    win.markDirty();
                    stateChanged = true;
                }
            }
        }
    } else if (event.type == core::InputEventType::PointerButton) {
        if (event.button == BTN_LEFT) {
            if (event.pressed) {
                const int titleH = core::DisplayScale::titleBarHeight();
                const int btn = core::DisplayScale::trafficBtn();
                const int btnPad = core::DisplayScale::px(10);
                for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                    auto& win = m_windows[i];
                    if (m_mouseX >= win.x && m_mouseX < win.x + win.width &&
                        m_mouseY >= win.y && m_mouseY < win.y + win.height) {

                        // Close button (red traffic light)
                        if (m_mouseX >= win.x + btnPad && m_mouseX <= win.x + btnPad + btn &&
                            m_mouseY >= win.y + btnPad && m_mouseY <= win.y + btnPad + btn) {
                            removeWindow(win.id);
                            stateChanged = true;
                            break;
                        }

                        focusWindow(win.id);
                        win.isDragging = (m_mouseY < win.y + titleH);
                        if (win.isDragging) {
                            win.dragOffsetX = m_mouseX - win.x;
                            win.dragOffsetY = m_mouseY - win.y;
                        }
                        win.markDirty();
                        stateChanged = true;
                        break;
                    }
                }
            } else {
                for (auto& win : m_windows) {
                    if (win.isDragging) {
                        win.isDragging = false;
                        stateChanged = true;
                    }
                }
            }
        }
    }
    return stateChanged;
}

void WindowManager::focusWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });

    if (it != m_windows.end()) {
        Window target = *it;
        m_windows.erase(it);

        for (auto& w : m_windows) {
            if (w.isFocused) {
                w.isFocused = false;
                w.headerColor = 0xFF45475A;
                w.markDirty();
            }
        }

        target.isFocused = true;
        target.headerColor = 0xFF89B4FA;
        target.markDirty();
        m_windows.push_back(target);
        m_mouseDirty = true;
    }
}

bool WindowManager::isAnyWindowDirty() const {
    if (m_mouseDirty) return true;
    for (const auto& w : m_windows) {
        if (w.isDirty) return true;
    }
    return false;
}

void WindowManager::markAllDirty() {
    m_mouseDirty = true;
    for (auto& w : m_windows) {
        w.markDirty();
    }
}

void WindowManager::clearAllDirty() {
    m_mouseDirty = false;
    for (auto& w : m_windows) {
        w.clearDirty();
    }
}

} // namespace lcl::render
