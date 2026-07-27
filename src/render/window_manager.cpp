#include "render/window_manager.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
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

    std::cout << "[LCL WindowManager] Initialized compositor canvas (" << m_screenWidth << "x" << m_screenHeight << ") [0 active surfaces].\n";
    return true;
}

void WindowManager::unfocusAll() {
    for (auto& w : m_windows) {
        if (w.isFocused) {
            w.isFocused = false;
            w.headerColor = lcl::theme::UI::WindowTitleBlurred;
            w.markDirty();
        }
    }
}

uint32_t WindowManager::createWindow(const std::string& title, int x, int y, int width, int height, uint32_t headerColor) {
    unfocusAll();

    Window win{};
    win.id = m_nextWindowId++;
    win.title = title;
    win.x = x;
    win.y = y;
    win.pendingX = x;
    win.pendingY = y;
    win.width = width;
    win.height = height;
    win.pendingWidth = width;
    win.pendingHeight = height;
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
            m_windows.back().headerColor = lcl::theme::UI::WindowTitleFocused;
        }
        markAllDirty();
        return true;
    }
    return false;
}

namespace {
    ResizeEdge detectResizeEdge(int mx, int my, const Window& win) {
        const int border = core::DisplayScale::px(8);
        bool nearLeft = (mx >= win.x - border && mx <= win.x + border);
        bool nearRight = (mx >= win.x + win.width - border && mx <= win.x + win.width + border);
        bool nearTop = (my >= win.y - border && my <= win.y + border);
        bool nearBottom = (my >= win.y + win.height - border && my <= win.y + win.height + border);

        if (nearTop && nearLeft) return ResizeEdge::TopLeft;
        if (nearTop && nearRight) return ResizeEdge::TopRight;
        if (nearBottom && nearLeft) return ResizeEdge::BottomLeft;
        if (nearBottom && nearRight) return ResizeEdge::BottomRight;
        if (nearLeft) return ResizeEdge::Left;
        if (nearRight) return ResizeEdge::Right;
        if (nearTop) return ResizeEdge::Top;
        if (nearBottom) return ResizeEdge::Bottom;

        return ResizeEdge::None;
    }
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

        const int menuH = core::DisplayScale::menuBarHeight();
        const int minW = core::DisplayScale::px(180);
        const int minH = core::DisplayScale::px(100);

        for (auto& win : m_windows) {
            // Dragging (Move)
            if (win.isDragging) {
                int newX = std::clamp(m_mouseX - win.dragOffsetX, 0, static_cast<int>(m_screenWidth) - win.width);
                int newY = std::clamp(m_mouseY - win.dragOffsetY, menuH, static_cast<int>(m_screenHeight) - win.height);
                if (newX != win.x || newY != win.y) {
                    win.x = newX;
                    win.y = newY;
                    win.pendingX = newX;
                    win.pendingY = newY;
                    win.markDirty();
                    stateChanged = true;
                }
            }
            // Resizing
            if (win.isResizing) {
                int deltaX = m_mouseX - win.resizeStartX;
                int deltaY = m_mouseY - win.resizeStartY;

                int newX = win.initialX;
                int newY = win.initialY;
                int newW = win.initialWidth;
                int newH = win.initialHeight;

                switch (win.resizeEdge) {
                    case ResizeEdge::Right:
                    case ResizeEdge::None: // Default Super + RightClick resize corner
                        newW = std::max(minW, win.initialWidth + deltaX);
                        break;
                    case ResizeEdge::Bottom:
                        newH = std::max(minH, win.initialHeight + deltaY);
                        break;
                    case ResizeEdge::Left: {
                        int candidateW = win.initialWidth - deltaX;
                        if (candidateW >= minW) {
                            newX = win.initialX + deltaX;
                            newW = candidateW;
                        }
                        break;
                    }
                    case ResizeEdge::Top: {
                        int candidateH = win.initialHeight - deltaY;
                        if (candidateH >= minH) {
                            newY = std::clamp(win.initialY + deltaY, menuH, static_cast<int>(m_screenHeight) - minH);
                            newH = win.initialHeight + (win.initialY - newY);
                        }
                        break;
                    }
                    case ResizeEdge::BottomRight:
                        newW = std::max(minW, win.initialWidth + deltaX);
                        newH = std::max(minH, win.initialHeight + deltaY);
                        break;
                    case ResizeEdge::BottomLeft: {
                        int candidateW = win.initialWidth - deltaX;
                        if (candidateW >= minW) {
                            newX = win.initialX + deltaX;
                            newW = candidateW;
                        }
                        newH = std::max(minH, win.initialHeight + deltaY);
                        break;
                    }
                    case ResizeEdge::TopRight: {
                        newW = std::max(minW, win.initialWidth + deltaX);
                        int candidateH = win.initialHeight - deltaY;
                        if (candidateH >= minH) {
                            newY = std::clamp(win.initialY + deltaY, menuH, static_cast<int>(m_screenHeight) - minH);
                            newH = win.initialHeight + (win.initialY - newY);
                        }
                        break;
                    }
                    case ResizeEdge::TopLeft: {
                        int candidateW = win.initialWidth - deltaX;
                        if (candidateW >= minW) {
                            newX = win.initialX + deltaX;
                            newW = candidateW;
                        }
                        int candidateH = win.initialHeight - deltaY;
                        if (candidateH >= minH) {
                            newY = std::clamp(win.initialY + deltaY, menuH, static_cast<int>(m_screenHeight) - minH);
                            newH = win.initialHeight + (win.initialY - newY);
                        }
                        break;
                    }
                }

                if (newX != win.pendingX || newY != win.pendingY || newW != win.pendingWidth || newH != win.pendingHeight) {
                    win.pendingX = newX;
                    win.pendingY = newY;
                    win.pendingWidth = newW;
                    win.pendingHeight = newH;
                    win.markDirty();
                    stateChanged = true;
                }
            }
        }
    } else if (event.type == core::InputEventType::PointerButton) {
        if (event.pressed) {
            // Strict Top-to-Bottom Z-Order Hit-Testing Bug Fix:
            // Find top-most target window under cursor FIRST without mutating array structure mid-loop!
            uint32_t targetWinId = 0;
            const int border = core::DisplayScale::px(8);

            for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                const auto& win = m_windows[i];
                if (m_mouseX >= win.x - border && m_mouseX < win.x + win.width + border &&
                    m_mouseY >= win.y - border && m_mouseY < win.y + win.height + border) {
                    targetWinId = win.id;
                    break;
                }
            }

            if (targetWinId > 0) {
                // Focus target window and bring to top z-order
                focusWindow(targetWinId);

                // Now m_windows.back() is guaranteed to be the focused top-most window
                auto& topWin = m_windows.back();
                const int titleH = core::DisplayScale::titleBarHeight();
                const int btn = core::DisplayScale::trafficBtn();
                const int btnPad = core::DisplayScale::px(10);

                // Close button check (only when explicitly clicked or Super shortcut used)
                if (event.superPressed && event.button == BTN_MIDDLE) {
                    removeWindow(topWin.id);
                    stateChanged = true;
                } else if (!event.superPressed &&
                           m_mouseX >= topWin.x + btnPad && m_mouseX <= topWin.x + btnPad + btn &&
                           m_mouseY >= topWin.y + btnPad && m_mouseY <= topWin.y + btnPad + btn) {
                    std::cout << "[LCL WM] Close button clicked on window ID: " << topWin.id << "\n";
                    removeWindow(topWin.id);
                    stateChanged = true;
                } else if (event.superPressed) {
                    // GNOME / KDE Style Super Shortcuts
                    if (event.button == BTN_LEFT) {
                        // Super + Left Click = Move
                        topWin.isDragging = true;
                        topWin.dragOffsetX = m_mouseX - topWin.x;
                        topWin.dragOffsetY = m_mouseY - topWin.y;
                        topWin.markDirty();
                        stateChanged = true;
                    } else if (event.button == BTN_RIGHT) {
                        // Super + Right Click = Normalized Aspect-Aware Grid (3x3 Dynamic Bounding Box Stretch)
                        topWin.isResizing = true;

                        // Calculate normalized click position in window local bounds [0.0, 1.0]
                        double normX = (topWin.width > 0)
                            ? static_cast<double>(m_mouseX - topWin.x) / static_cast<double>(topWin.width)
                            : 0.5;
                        double normY = (topWin.height > 0)
                            ? static_cast<double>(m_mouseY - topWin.y) / static_cast<double>(topWin.height)
                            : 0.5;

                        // 3x3 Aspect-Aware Bounding Box Grid Mapping (Thresholds: 0.33 & 0.66)
                        if (normY < 0.33) {
                            if (normX < 0.33)      topWin.resizeEdge = ResizeEdge::TopLeft;
                            else if (normX > 0.66) topWin.resizeEdge = ResizeEdge::TopRight;
                            else                   topWin.resizeEdge = ResizeEdge::Top;
                        } else if (normY > 0.66) {
                            if (normX < 0.33)      topWin.resizeEdge = ResizeEdge::BottomLeft;
                            else if (normX > 0.66) topWin.resizeEdge = ResizeEdge::BottomRight;
                            else                   topWin.resizeEdge = ResizeEdge::Bottom;
                        } else {
                            if (normX < 0.33)      topWin.resizeEdge = ResizeEdge::Left;
                            else if (normX > 0.66) topWin.resizeEdge = ResizeEdge::Right;
                            else                   topWin.resizeEdge = ResizeEdge::BottomRight; // Center default
                        }

                        topWin.resizeStartX = m_mouseX;
                        topWin.resizeStartY = m_mouseY;
                        topWin.initialX = topWin.x;
                        topWin.initialY = topWin.y;
                        topWin.initialWidth = topWin.width;
                        topWin.initialHeight = topWin.height;
                        topWin.markDirty();
                        stateChanged = true;
                    }
                } else if (event.button == BTN_LEFT) {
                    // Normal Left Click
                    ResizeEdge edge = detectResizeEdge(m_mouseX, m_mouseY, topWin);
                    if (edge != ResizeEdge::None) {
                        // Edge / Corner Resize
                        topWin.isResizing = true;
                        topWin.resizeEdge = edge;
                        topWin.resizeStartX = m_mouseX;
                        topWin.resizeStartY = m_mouseY;
                        topWin.initialX = topWin.x;
                        topWin.initialY = topWin.y;
                        topWin.initialWidth = topWin.width;
                        topWin.initialHeight = topWin.height;
                        topWin.markDirty();
                        stateChanged = true;
                    } else if (m_mouseY < topWin.y + titleH) {
                        // Header Drag Move
                        topWin.isDragging = true;
                        topWin.dragOffsetX = m_mouseX - topWin.x;
                        topWin.dragOffsetY = m_mouseY - topWin.y;
                        topWin.markDirty();
                        stateChanged = true;
                    }
                }
            }
        } else {
            // Button Released: Release dragging and resizing for all windows
            for (auto& win : m_windows) {
                if (win.isDragging || win.isResizing) {
                    win.isDragging = false;
                    win.isResizing = false;
                    win.resizeEdge = ResizeEdge::None;
                    win.markDirty();
                    stateChanged = true;
                }
            }
        }
    }
    return stateChanged;
}

void WindowManager::commitSurfaceGeometry(uint32_t windowId, int frameW, int frameH) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end()) return;

    Window& win = *it;
    int finalX = win.x;
    int finalY = win.y;

    // Generic opposite-edge anchor offset for client surface buffers applies ONLY during window RESIZE
    if (win.isResizing) {
        finalX = win.pendingX;
        finalY = win.pendingY;

        int diffW = win.pendingWidth - frameW;
        int diffH = win.pendingHeight - frameH;

        if (win.resizeEdge == ResizeEdge::Left ||
            win.resizeEdge == ResizeEdge::TopLeft ||
            win.resizeEdge == ResizeEdge::BottomLeft) {
            finalX += diffW;
        }
        if (win.resizeEdge == ResizeEdge::Top ||
            win.resizeEdge == ResizeEdge::TopLeft ||
            win.resizeEdge == ResizeEdge::TopRight) {
            finalY += diffH;
        }
    }

    if (win.width != frameW || win.height != frameH || win.x != finalX || win.y != finalY) {
        win.width = frameW;
        win.height = frameH;
        win.x = finalX;
        win.y = finalY;
        win.pendingX = finalX;
        win.pendingY = finalY;
        win.pendingWidth = frameW;
        win.pendingHeight = frameH;
        win.markDirty();
    }
}

void WindowManager::setDecorationMode(uint32_t windowId, DecorationMode mode) {
    for (auto& win : m_windows) {
        if (win.id == windowId) {
            win.decorationMode = mode;
            win.markDirty();
            m_mouseDirty = true;
            break;
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

        unfocusAll();

        target.isFocused = true;
        target.headerColor = lcl::theme::UI::WindowTitleFocused;
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
