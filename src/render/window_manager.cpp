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
    m_subpixelX = static_cast<double>(m_mouseX);
    m_subpixelY = static_cast<double>(m_mouseY);
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
    sortWindowsByLayer();
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
            for (auto revIt = m_windows.rbegin(); revIt != m_windows.rend(); ++revIt) {
                if (!revIt->isUnfocusable) {
                    revIt->isFocused = true;
                    revIt->headerColor = lcl::theme::UI::WindowTitleFocused;
                    break;
                }
            }
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

        if (event.absoluteX >= 0.0) {
            m_subpixelX = event.absoluteX;
        }
        if (event.absoluteY >= 0.0) {
            m_subpixelY = event.absoluteY;
        }

        if (event.absoluteX < 0.0 && event.absoluteY < 0.0) {
            // Relative mouse motion: apply subpixel precision + speed sensitivity scale & acceleration
            constexpr double mouseSensitivity = 1.8;
            double dx = event.dx * mouseSensitivity;
            double dy = event.dy * mouseSensitivity;

            // Non-linear acceleration for fast flick movements
            double speedSq = dx * dx + dy * dy;
            if (speedSq > 9.0) {
                double factor = 1.0 + std::min(1.5, (speedSq - 9.0) * 0.01);
                dx *= factor;
                dy *= factor;
            }

            m_subpixelX += dx;
            m_subpixelY += dy;
        }

        m_subpixelX = std::clamp(m_subpixelX, 0.0, static_cast<double>(m_screenWidth - 1));
        m_subpixelY = std::clamp(m_subpixelY, 0.0, static_cast<double>(m_screenHeight - 1));
        m_mouseX = static_cast<int>(m_subpixelX);
        m_mouseY = static_cast<int>(m_subpixelY);

        if (m_mouseX != oldX || m_mouseY != oldY || event.dx != 0.0 || event.dy != 0.0 || event.absoluteX >= 0.0) {
            m_mouseDirty = true;
            stateChanged = true;
        }

        const int topInset = std::max(core::DisplayScale::menuBarHeight(), static_cast<int>(m_reservedZone.top));
        const int bottomInset = static_cast<int>(m_reservedZone.bottom);
        const int minW = core::DisplayScale::px(180);
        const int minH = core::DisplayScale::px(100);

        for (auto& win : m_windows) {
            // Dragging (Move)
            if (win.isDragging) {
                int newX = std::clamp(m_mouseX - win.dragOffsetX, 0, static_cast<int>(m_screenWidth) - win.width);
                int newY = std::clamp(m_mouseY - win.dragOffsetY, topInset, static_cast<int>(m_screenHeight) - win.height - bottomInset);
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
                            newY = std::clamp(win.initialY + deltaY, topInset, static_cast<int>(m_screenHeight) - minH);
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
                            newY = std::clamp(win.initialY + deltaY, topInset, static_cast<int>(m_screenHeight) - minH);
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
                            newY = std::clamp(win.initialY + deltaY, topInset, static_cast<int>(m_screenHeight) - minH);
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
            // Find top-most interactable window under cursor FIRST without mutating array structure mid-loop!
            uint32_t targetWinId = 0;
            const int border = core::DisplayScale::px(8);

            for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                const auto& win = m_windows[i];
                if (win.isUnfocusable || win.layer == protocol::LCLWindowLayer::Bottom) {
                    continue; // Skip unfocusable background surfaces (e.g. Wallpaper)
                }
                if (m_mouseX >= win.x - border && m_mouseX < win.x + win.width + border &&
                    m_mouseY >= win.y - border && m_mouseY < win.y + win.height + border) {
                    targetWinId = win.id;
                    break;
                }
            }

            if (targetWinId > 0) {
                // Focus target window and bring to top z-order within its layer
                focusWindow(targetWinId);

                // Find the target window directly by ID (layer sorting may keep it in Normal/TopMost layer)
                auto targetIt = std::find_if(m_windows.begin(), m_windows.end(), [targetWinId](const Window& w) {
                    return w.id == targetWinId;
                });

                if (targetIt != m_windows.end()) {
                    auto& targetWin = *targetIt;
                    const int titleH = (targetWin.decorationMode == DecorationMode::SSD) ? core::DisplayScale::titleBarHeight() : 0;
                    const int btn = core::DisplayScale::trafficBtn();
                    const int btnPad = core::DisplayScale::px(10);

                    // Close button check (only when explicitly clicked or Super shortcut used)
                    if (event.superPressed && event.button == BTN_MIDDLE) {
                        removeWindow(targetWin.id);
                        stateChanged = true;
                    } else if (!event.superPressed && targetWin.decorationMode == DecorationMode::SSD &&
                               m_mouseX >= targetWin.x + btnPad && m_mouseX <= targetWin.x + btnPad + btn &&
                               m_mouseY >= targetWin.y + btnPad && m_mouseY <= targetWin.y + btnPad + btn) {
                        std::cout << "[LCL WM] Close button clicked on window ID: " << targetWin.id << "\n";
                        removeWindow(targetWin.id);
                        stateChanged = true;
                    } else if (event.superPressed) {
                        // GNOME / KDE Style Super Shortcuts
                        if (event.button == BTN_LEFT) {
                            // Super + Left Click = Move
                            targetWin.isDragging = true;
                            targetWin.dragOffsetX = m_mouseX - targetWin.x;
                            targetWin.dragOffsetY = m_mouseY - targetWin.y;
                            targetWin.markDirty();
                            stateChanged = true;
                        } else if (event.button == BTN_RIGHT) {
                            // Super + Right Click = Normalized Aspect-Aware Grid
                            targetWin.isResizing = true;

                            double normX = (targetWin.width > 0)
                                ? static_cast<double>(m_mouseX - targetWin.x) / static_cast<double>(targetWin.width)
                                : 0.5;
                            double normY = (targetWin.height > 0)
                                ? static_cast<double>(m_mouseY - targetWin.y) / static_cast<double>(targetWin.height)
                                : 0.5;

                            if (normY < 0.33) {
                                if (normX < 0.33)      targetWin.resizeEdge = ResizeEdge::TopLeft;
                                else if (normX > 0.66) targetWin.resizeEdge = ResizeEdge::TopRight;
                                else                   targetWin.resizeEdge = ResizeEdge::Top;
                            } else if (normY > 0.66) {
                                if (normX < 0.33)      targetWin.resizeEdge = ResizeEdge::BottomLeft;
                                else if (normX > 0.66) targetWin.resizeEdge = ResizeEdge::BottomRight;
                                else                   targetWin.resizeEdge = ResizeEdge::Bottom;
                            } else {
                                if (normX < 0.33)      targetWin.resizeEdge = ResizeEdge::Left;
                                else if (normX > 0.66) targetWin.resizeEdge = ResizeEdge::Right;
                                else                   targetWin.resizeEdge = ResizeEdge::BottomRight;
                            }

                            targetWin.activeResizeEdge = targetWin.resizeEdge;
                            targetWin.anchorRight = targetWin.x + targetWin.width;
                            targetWin.anchorBottom = targetWin.y + targetWin.height;
                            targetWin.resizeStartX = m_mouseX;
                            targetWin.resizeStartY = m_mouseY;
                            targetWin.initialX = targetWin.x;
                            targetWin.initialY = targetWin.y;
                            targetWin.initialWidth = targetWin.width;
                            targetWin.initialHeight = targetWin.height;
                            targetWin.markDirty();
                            stateChanged = true;
                        }
                    } else if (event.button == BTN_LEFT) {
                        // Normal Left Click
                        ResizeEdge edge = detectResizeEdge(m_mouseX, m_mouseY, targetWin);
                        if (edge != ResizeEdge::None) {
                            // Edge / Corner Resize
                            targetWin.isResizing = true;
                            targetWin.resizeEdge = edge;
                            targetWin.activeResizeEdge = edge;
                            targetWin.anchorRight = targetWin.x + targetWin.width;
                            targetWin.anchorBottom = targetWin.y + targetWin.height;
                            targetWin.resizeStartX = m_mouseX;
                            targetWin.resizeStartY = m_mouseY;
                            targetWin.initialX = targetWin.x;
                            targetWin.initialY = targetWin.y;
                            targetWin.initialWidth = targetWin.width;
                            targetWin.initialHeight = targetWin.height;
                            targetWin.markDirty();
                            stateChanged = true;
                        } else if (titleH > 0 && m_mouseY < targetWin.y + titleH) {
                            // Header Drag Move
                            targetWin.isDragging = true;
                            targetWin.dragOffsetX = m_mouseX - targetWin.x;
                            targetWin.dragOffsetY = m_mouseY - targetWin.y;
                            targetWin.markDirty();
                            stateChanged = true;
                        }
                    }
                }
            } else {
                // Clicked on empty desktop background
                unfocusAll();
                stateChanged = true;
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

    // Use activeResizeEdge (remains active across client commits until queue is fully drained)
    ResizeEdge edgeToUse = (win.isResizing ? win.resizeEdge : win.activeResizeEdge);

    if (edgeToUse != ResizeEdge::None) {
        int rightAnchor = (win.anchorRight > 0 ? win.anchorRight : (win.isResizing ? win.pendingX + win.pendingWidth : win.x + win.width));
        int bottomAnchor = (win.anchorBottom > 0 ? win.anchorBottom : (win.isResizing ? win.pendingY + win.pendingHeight : win.y + win.height));

        // Sol kenar sürüklendiyse: Sağ kenar (rightAnchor) sabittir!
        if (edgeToUse == ResizeEdge::Left ||
            edgeToUse == ResizeEdge::TopLeft ||
            edgeToUse == ResizeEdge::BottomLeft) {
            finalX = rightAnchor - frameW;
        } else if (win.isResizing) {
            finalX = win.pendingX;
        }

        // Üst kenar sürüklendiyse: Alt kenar (bottomAnchor) sabittir!
        if (edgeToUse == ResizeEdge::Top ||
            edgeToUse == ResizeEdge::TopLeft ||
            edgeToUse == ResizeEdge::TopRight) {
            finalY = bottomAnchor - frameH;
        } else if (win.isResizing) {
            finalY = win.pendingY;
        }

        // activeResizeEdge sıfırlanma kuralı: Mouse bırakıldıysa VE gelen tampon hedefe ulaştıysa (veya son karedir)
        if (!win.isResizing && (frameW == win.pendingWidth || frameH == win.pendingHeight)) {
            win.activeResizeEdge = ResizeEdge::None;
            win.anchorRight = 0;
            win.anchorBottom = 0;
        }
    }

    if (win.width != frameW || win.height != frameH || win.x != finalX || win.y != finalY) {
        win.width = frameW;
        win.height = frameH;
        win.x = finalX;
        win.y = finalY;
        win.pendingX = finalX;
        win.pendingY = finalY;
        if (!win.isResizing) {
            win.pendingWidth = frameW;
            win.pendingHeight = frameH;
        }
        win.markDirty();
    }
}

void WindowManager::sortWindowsByLayer() {
    std::stable_sort(m_windows.begin(), m_windows.end(), [](const Window& a, const Window& b) {
        return static_cast<uint32_t>(a.layer) < static_cast<uint32_t>(b.layer);
    });
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

void WindowManager::setWindowLayer(uint32_t windowId, protocol::LCLWindowLayer layer, bool unfocusable) {
    for (auto& win : m_windows) {
        if (win.id == windowId) {
            win.layer = layer;
            win.isUnfocusable = unfocusable;
            if (unfocusable && win.isFocused) {
                win.isFocused = false;
                win.headerColor = lcl::theme::UI::WindowTitleBlurred;
            }
            win.markDirty();
            break;
        }
    }
    sortWindowsByLayer();
    m_mouseDirty = true;
}

void WindowManager::setReservedZone(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    m_reservedZone = {top, bottom, left, right};
    std::cout << "[LCL WindowManager] Reserved Zone set to top=" << top << " bottom=" << bottom << " left=" << left << " right=" << right << "\n";
}

void WindowManager::focusWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });

    if (it != m_windows.end()) {
        Window target = *it;
        m_windows.erase(it);

        if (!target.isUnfocusable) {
            unfocusAll();
            target.isFocused = true;
            target.headerColor = lcl::theme::UI::WindowTitleFocused;
        }

        target.markDirty();
        m_windows.push_back(target);
        sortWindowsByLayer();
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
