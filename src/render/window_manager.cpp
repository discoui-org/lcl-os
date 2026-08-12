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
    m_motionEngine.clearAll();
    m_chromeMotionEngine.clearAll();
    m_initialized = true;
    m_lastAnimTick = std::chrono::steady_clock::now();

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

uint32_t WindowManager::createWindow(const std::string& title, int x, int y, int width, int height,
                                     uint32_t headerColor, bool focus) {
    if (focus) {
        unfocusAll();
    }

    const int topInset = static_cast<int>(m_reservedZone.top);
    int clampedY = y;
    if (clampedY < topInset) {
        clampedY = topInset;
    }

    Window win{};
    win.id = m_nextWindowId++;
    win.title = title;
    win.x = x;
    win.y = clampedY;
    win.pendingX = x;
    win.pendingY = clampedY;
    win.width = width;
    win.height = height;
    win.pendingWidth = width;
    win.pendingHeight = height;
    win.presentationX = static_cast<float>(x);
    win.presentationY = static_cast<float>(clampedY);
    win.presentationWidth = static_cast<float>(width);
    win.presentationHeight = static_cast<float>(height);
    win.presentationInitialized = true;
    win.headerColor = headerColor;
    win.isFocused = focus;
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
        m_motionEngine.clearObjectChannels(windowId);
        m_chromeMotionEngine.clearObjectChannels(windowId);

        if (!m_windows.empty()) {
            for (auto revIt = m_windows.rbegin(); revIt != m_windows.rend(); ++revIt) {
                if (!revIt->isUnfocusable && !revIt->isMinimized) {
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

    int hitWindowChromeControl(const Window& win, int mouseX, int mouseY) {
        if (win.decorationMode != DecorationMode::SSD) return -1;

        const float scale = core::DisplayScale::factor();
        const float cornerRadius = win.cornerRadiusPx >= 0.0f
            ? win.cornerRadiusPx
            : 20.0f * scale;
        const float controlLeftInset = std::max({8.0f * scale,
                                                 cornerRadius - 8.0f * scale,
                                                 4.0f * scale});
        const float controlSizeF = 16.0f * scale;
        const float controlGap = 6.0f * scale;
        const int controlTop = win.y + static_cast<int>(std::lround(controlLeftInset));
        const int controlSize = std::max(1, static_cast<int>(std::lround(controlSizeF)));
        if (mouseY < controlTop || mouseY > controlTop + controlSize) return -1;

        for (int index = 0; index < 3; ++index) {
            const float left = controlLeftInset +
                static_cast<float>(index) * (controlSizeF + controlGap);
            const int controlLeft = win.x + static_cast<int>(std::lround(left));
            if (mouseX >= controlLeft && mouseX <= controlLeft + controlSize) {
                return index;
            }
        }
        return -1;
    }
}

void WindowManager::setChromeControlState(Window& window, int hoveredControl,
                                          int pressedControl) {
    hoveredControl = std::clamp(hoveredControl, -1, 2);
    pressedControl = std::clamp(pressedControl, -1, 2);
    const int oldHovered = window.hoveredChromeControl;
    const int oldPressed = window.pressedChromeControl;
    if (oldHovered == hoveredControl && oldPressed == pressedControl) return;

    window.hoveredChromeControl = hoveredControl;
    window.pressedChromeControl = pressedControl;
    for (int index = 0; index < 3; ++index) {
        const float oldEmphasis = index == oldPressed ? 2.0f : (index == oldHovered ? 1.0f : 0.0f);
        const float targetEmphasis = index == pressedControl ? 2.0f : (index == hoveredControl ? 1.0f : 0.0f);
        if (oldEmphasis == targetEmphasis) continue;

        const float targetScale = targetEmphasis >= 2.0f
            ? 0.965f
            : (targetEmphasis >= 1.0f ? 1.015f : 1.0f);
        const lcl::motion::Motion motion = targetEmphasis >= 2.0f
            ? lcl::motion::tokens::pressed()
            : (targetEmphasis >= 1.0f ? lcl::motion::tokens::hover()
                                      : lcl::motion::tokens::release());
        const uint32_t propertyBase = 100u + static_cast<uint32_t>(index) * 2u;
        const auto scaleChannel = m_chromeMotionEngine.ensureChannel(
            {window.id, propertyBase}, window.chromeControlScale[index]);
        const auto emphasisChannel = m_chromeMotionEngine.ensureChannel(
            {window.id, propertyBase + 1u}, window.chromeControlEmphasis[index]);
        m_chromeMotionEngine.animateTo(scaleChannel, targetScale, motion);
        m_chromeMotionEngine.animateTo(emphasisChannel, targetEmphasis, motion);
    }
    window.markDirty();
}

void WindowManager::refreshChromeHoverState() {
    uint32_t hoveredWindowId = 0;
    int hoveredControl = -1;
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        const auto& window = *it;
        if (window.isMinimized || window.isUnfocusable ||
            window.layer == protocol::LCLWindowLayer::Bottom) {
            continue;
        }
        if (m_mouseX < window.x || m_mouseX >= window.x + window.width ||
            m_mouseY < window.y || m_mouseY >= window.y + window.height) {
            continue;
        }
        if (!window.geometryTransitionActive) {
            hoveredControl = hitWindowChromeControl(window, m_mouseX, m_mouseY);
            if (hoveredControl >= 0) hoveredWindowId = window.id;
        }
        break;
    }

    for (auto& window : m_windows) {
        const int nextHover = window.id == hoveredWindowId ? hoveredControl : -1;
        setChromeControlState(window, nextHover, window.pressedChromeControl);
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
            refreshChromeHoverState();
        }

        const int topInset = static_cast<int>(m_reservedZone.top);
        const int bottomInset = static_cast<int>(m_reservedZone.bottom);
        const int leftInset = static_cast<int>(m_reservedZone.left);
        const int rightInset = static_cast<int>(m_reservedZone.right);
        const int minW = core::DisplayScale::px(180);
        const int minH = core::DisplayScale::px(100);
        const int maxW = std::max(minW, static_cast<int>(m_screenWidth) - leftInset - rightInset);
        const int maxH = std::max(minH, static_cast<int>(m_screenHeight) - topInset - bottomInset);

        for (auto& win : m_windows) {
            // Resizing has priority and must never mix with drag updates.
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
                        newW = std::clamp(win.initialWidth + deltaX, minW, maxW);
                        break;
                    case ResizeEdge::Bottom:
                        newH = std::clamp(win.initialHeight + deltaY, minH, maxH);
                        break;
                    case ResizeEdge::Left: {
                        int candidateW = win.initialWidth - deltaX;
                        newW = std::clamp(candidateW, minW, maxW);
                        newX = win.initialX + (win.initialWidth - newW);
                        break;
                    }
                    case ResizeEdge::Top: {
                        const int bottomAnchor = win.initialY + win.initialHeight;
                        int candidateH = win.initialHeight - deltaY;
                        newH = std::clamp(candidateH, minH, maxH);
                        newY = bottomAnchor - newH;
                        newY = std::clamp(newY, topInset, static_cast<int>(m_screenHeight) - minH);
                        newH = bottomAnchor - newY;
                        newH = std::clamp(newH, minH, maxH);
                        break;
                    }
                    case ResizeEdge::BottomRight:
                        newW = std::clamp(win.initialWidth + deltaX, minW, maxW);
                        newH = std::clamp(win.initialHeight + deltaY, minH, maxH);
                        break;
                    case ResizeEdge::BottomLeft: {
                        int candidateW = win.initialWidth - deltaX;
                        newW = std::clamp(candidateW, minW, maxW);
                        newX = win.initialX + (win.initialWidth - newW);
                        newH = std::clamp(win.initialHeight + deltaY, minH, maxH);
                        break;
                    }
                    case ResizeEdge::TopRight: {
                        const int bottomAnchor = win.initialY + win.initialHeight;
                        newW = std::clamp(win.initialWidth + deltaX, minW, maxW);
                        int candidateH = win.initialHeight - deltaY;
                        newH = std::clamp(candidateH, minH, maxH);
                        newY = bottomAnchor - newH;
                        newY = std::clamp(newY, topInset, static_cast<int>(m_screenHeight) - minH);
                        newH = bottomAnchor - newY;
                        newH = std::clamp(newH, minH, maxH);
                        break;
                    }
                    case ResizeEdge::TopLeft: {
                        const int bottomAnchor = win.initialY + win.initialHeight;
                        int candidateW = win.initialWidth - deltaX;
                        newW = std::clamp(candidateW, minW, maxW);
                        newX = win.initialX + (win.initialWidth - newW);
                        int candidateH = win.initialHeight - deltaY;
                        newH = std::clamp(candidateH, minH, maxH);
                        newY = bottomAnchor - newH;
                        newY = std::clamp(newY, topInset, static_cast<int>(m_screenHeight) - minH);
                        newH = bottomAnchor - newY;
                        newH = std::clamp(newH, minH, maxH);
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
            } else if (win.isDragging) {
                int newX = m_mouseX - win.dragOffsetX;
                int newY = std::max(topInset, m_mouseY - win.dragOffsetY);

                win.lastDragVelX = static_cast<float>(newX - win.x);
                win.lastDragVelY = static_cast<float>(newY - win.y);
                win.snapBackActive = false;

                if (newX != win.x || newY != win.y) {
                    win.x = newX;
                    win.y = newY;
                    win.presentationX = static_cast<float>(newX);
                    win.presentationY = static_cast<float>(newY);
                    win.pendingX = newX;
                    win.pendingY = newY;
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
            bool blockedByTransition = false;
            const int border = core::DisplayScale::px(8);

            for (int i = static_cast<int>(m_windows.size()) - 1; i >= 0; --i) {
                const auto& win = m_windows[i];
                if (win.geometryTransitionActive) {
                    if (m_mouseX >= win.presentationX &&
                        m_mouseX < win.presentationX + win.presentationWidth &&
                        m_mouseY >= win.presentationY &&
                        m_mouseY < win.presentationY + win.presentationHeight) {
                        blockedByTransition = true;
                        break;
                    }
                    continue;
                }
                if (win.isUnfocusable || win.isMinimized ||
                    win.layer == protocol::LCLWindowLayer::Bottom) {
                    continue; // Skip unfocusable background surfaces (e.g. Wallpaper)
                }
                if (m_mouseX >= win.x - border && m_mouseX < win.x + win.width + border &&
                    m_mouseY >= win.y - border && m_mouseY < win.y + win.height + border) {
                    targetWinId = win.id;
                    break;
                }
            }

            if (blockedByTransition) {
                return stateChanged;
            } else if (targetWinId > 0) {
                // Focus target window and bring to top z-order within its layer
                focusWindow(targetWinId);

                // Find the target window directly by ID (layer sorting may keep it in Normal/TopMost layer)
                auto targetIt = std::find_if(m_windows.begin(), m_windows.end(), [targetWinId](const Window& w) {
                    return w.id == targetWinId;
                });

                if (targetIt != m_windows.end()) {
                    auto& targetWin = *targetIt;
                    const int titleH = (targetWin.decorationMode == DecorationMode::SSD) ? core::DisplayScale::titleBarHeight() : 0;
                    const int chromeControl = hitWindowChromeControl(targetWin, m_mouseX, m_mouseY);
                    if (chromeControl >= 0 && !event.superPressed) {
                        setChromeControlState(targetWin, chromeControl, chromeControl);
                    }

                    // Controls share the exact WindowChrome layout used to draw SSD.
                    if (event.superPressed && event.button == BTN_MIDDLE) {
                        targetWin.closeRequested = true;
                        targetWin.markDirty();
                        stateChanged = true;
                    } else if (!event.superPressed && chromeControl == 0) {
                        std::cout << "[LCL WM] Close button clicked on window ID: " << targetWin.id << "\n";
                        targetWin.closeRequested = true;
                        targetWin.markDirty();
                        stateChanged = true;
                    } else if (!event.superPressed && chromeControl == 1) {
                        stateChanged = minimizeWindow(targetWin.id) || stateChanged;
                    } else if (!event.superPressed && chromeControl == 2) {
                        stateChanged = toggleMaximizeWindow(targetWin.id) || stateChanged;
                    } else if (event.superPressed) {
                        // GNOME / KDE Style Super Shortcuts
                        if (event.button == BTN_LEFT) {
                            // Super + Left Click = Move
                            targetWin.isDragging = true;
                            targetWin.isResizing = false;
                            targetWin.snapBackActive = false;
                            targetWin.snapVelX = 0.0f;
                            targetWin.snapVelY = 0.0f;
                            targetWin.dragOffsetX = m_mouseX - targetWin.x;
                            targetWin.dragOffsetY = m_mouseY - targetWin.y;
                            targetWin.markDirty();
                            stateChanged = true;
                        } else if (event.button == BTN_RIGHT) {
                            // Super + Right Click = Normalized Aspect-Aware Grid
                            targetWin.isDragging = false;
                            targetWin.isResizing = true;
                            targetWin.snapBackActive = false;
                            targetWin.snapVelX = 0.0f;
                            targetWin.snapVelY = 0.0f;

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
                            targetWin.isDragging = false;
                            targetWin.isResizing = true;
                            targetWin.snapBackActive = false;
                            targetWin.snapVelX = 0.0f;
                            targetWin.snapVelY = 0.0f;
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
                            targetWin.isResizing = false;
                            targetWin.snapBackActive = false;
                            targetWin.snapVelX = 0.0f;
                            targetWin.snapVelY = 0.0f;
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
            constexpr int kVisibleSafePx = 50;
            const int safeLeft = static_cast<int>(m_reservedZone.left);
            const int safeTop = static_cast<int>(m_reservedZone.top);
            const int safeRight = static_cast<int>(m_screenWidth) - static_cast<int>(m_reservedZone.right);
            const int safeBottom = static_cast<int>(m_screenHeight) - static_cast<int>(m_reservedZone.bottom);

            for (auto& win : m_windows) {
                if (win.pressedChromeControl >= 0) {
                    setChromeControlState(win, win.hoveredChromeControl, -1);
                    stateChanged = true;
                }
                if (win.isDragging || win.isResizing) {
                    const bool wasResizing = win.isResizing;
                    const bool wasDragging = win.isDragging;
                    if (wasResizing || wasDragging) {
                        const int minSafeX = safeLeft - std::max(0, win.width - kVisibleSafePx);
                        const int maxSafeX = safeRight - kVisibleSafePx;
                        const int maxSafeY = safeBottom - kVisibleSafePx;

                        const float targetX = static_cast<float>(std::clamp(win.x, minSafeX, maxSafeX));
                        const float targetY = static_cast<float>(std::clamp(win.y, safeTop, maxSafeY));

                        if (std::abs(targetX - static_cast<float>(win.x)) > 0.5f ||
                            std::abs(targetY - static_cast<float>(win.y)) > 0.5f) {
                            win.snapBackActive = true;
                            win.snapX = static_cast<float>(win.x);
                            win.snapY = static_cast<float>(win.y);
                            win.snapTargetX = targetX;
                            win.snapTargetY = targetY;
                            if (wasDragging) {
                                win.snapVelX = win.lastDragVelX * 25.0f;
                                win.snapVelY = win.lastDragVelY * 25.0f;
                            } else {
                                win.snapVelX = 0.0f;
                                win.snapVelY = 0.0f;
                            }
                            m_motionEngine.clearObjectChannels(win.id);
                            auto motion = lcl::motion::tokens::dragSnapBack();
                            motion.springParams.initialVelocity = win.snapVelX;
                            const auto xChannel = m_motionEngine.createChannel({win.id, 1}, win.snapX);
                            m_motionEngine.animateTo(xChannel, win.snapTargetX, motion);
                            motion.springParams.initialVelocity = win.snapVelY;
                            const auto yChannel = m_motionEngine.createChannel({win.id, 2}, win.snapY);
                            m_motionEngine.animateTo(yChannel, win.snapTargetY, motion);
                        }
                    }

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

bool WindowManager::updateAnimations() {
    if (!m_initialized) return false;

    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastAnimTick).count();
    m_lastAnimTick = now;
    return updateAnimations(dt);
}

bool WindowManager::updateAnimations(float dt) {
    if (!m_initialized) return false;
    if (dt <= 0.0f) return false;
    bool changed = false;
    const auto changedChannels = m_motionEngine.tick(dt);
    (void)changedChannels;
    m_chromeMotionEngine.tick(dt);

    for (auto& win : m_windows) {
        for (int index = 0; index < 3; ++index) {
            const uint32_t propertyBase = 100u + static_cast<uint32_t>(index) * 2u;
            const auto scaleChannel = m_chromeMotionEngine.findChannel({win.id, propertyBase});
            const auto emphasisChannel = m_chromeMotionEngine.findChannel({win.id, propertyBase + 1u});
            if (!scaleChannel || !emphasisChannel) continue;
            const float scale = m_chromeMotionEngine.sample(*scaleChannel).value;
            const float emphasis = m_chromeMotionEngine.sample(*emphasisChannel).value;
            if (std::fabs(scale - win.chromeControlScale[index]) > 0.0001f ||
                std::fabs(emphasis - win.chromeControlEmphasis[index]) > 0.0001f) {
                win.chromeControlScale[index] = scale;
                win.chromeControlEmphasis[index] = emphasis;
                win.markDirty();
                changed = true;
            }
        }
        if (win.geometryTransitionActive) {
            const Rect before{static_cast<int>(std::lround(win.presentationX)),
                              static_cast<int>(std::lround(win.presentationY)),
                              static_cast<int>(std::lround(win.presentationWidth)),
                              static_cast<int>(std::lround(win.presentationHeight))};
            const auto x = m_motionEngine.findChannel({win.id, 10});
            const auto y = m_motionEngine.findChannel({win.id, 11});
            const auto width = m_motionEngine.findChannel({win.id, 12});
            const auto height = m_motionEngine.findChannel({win.id, 13});
            if (x && y && width && height) {
                const auto sx = m_motionEngine.sample(*x);
                const auto sy = m_motionEngine.sample(*y);
                const auto sw = m_motionEngine.sample(*width);
                const auto sh = m_motionEngine.sample(*height);
                win.presentationX = sx.value;
                win.presentationY = sy.value;
                win.presentationWidth = sw.value;
                win.presentationHeight = sh.value;
                win.geometryTransitionActive = sx.active || sy.active || sw.active || sh.active;
                const Rect after{static_cast<int>(std::lround(win.presentationX)),
                                 static_cast<int>(std::lround(win.presentationY)),
                                 static_cast<int>(std::lround(win.presentationWidth)),
                                 static_cast<int>(std::lround(win.presentationHeight))};
                win.markDirty(Rect::Union(before, after));
                changed = true;
            } else {
                win.geometryTransitionActive = false;
            }
        }
        if (!win.snapBackActive) continue;

        if (win.isDragging || win.isResizing) {
            win.snapBackActive = false;
            m_motionEngine.clearObjectChannels(win.id);
            win.snapVelX = 0.0f;
            win.snapVelY = 0.0f;
            continue;
        }

        const auto xChannel = m_motionEngine.findChannel({win.id, 1});
        const auto yChannel = m_motionEngine.findChannel({win.id, 2});
        if (!xChannel || !yChannel) { win.snapBackActive = false; continue; }
        const auto xSample = m_motionEngine.sample(*xChannel);
        const auto ySample = m_motionEngine.sample(*yChannel);
        win.snapX = xSample.value;
        win.snapY = ySample.value;
        win.snapVelX = xSample.velocity;
        win.snapVelY = ySample.velocity;

        const int newX = static_cast<int>(std::lround(win.snapX));
        const int newY = static_cast<int>(std::lround(win.snapY));

        if (newX != win.x || newY != win.y) {
            win.x = newX;
            win.y = newY;
            win.pendingX = newX;
            win.pendingY = newY;
            win.presentationX = win.snapX;
            win.presentationY = win.snapY;
            win.markDirty();
            changed = true;
        }

        if (!xSample.active && !ySample.active) {
            win.snapX = win.snapTargetX;
            win.snapY = win.snapTargetY;
            win.x = static_cast<int>(std::lround(win.snapTargetX));
            win.y = static_cast<int>(std::lround(win.snapTargetY));
            win.pendingX = win.x;
            win.pendingY = win.y;
            win.presentationX = win.snapX;
            win.presentationY = win.snapY;
            win.snapVelX = 0.0f;
            win.snapVelY = 0.0f;
            win.snapBackActive = false;
            m_motionEngine.clearObjectChannels(win.id);
            win.markDirty();
            changed = true;
        }
    }

    if (changed) {
        m_mouseDirty = true;
    }

    return changed;
}

void WindowManager::commitSurfaceGeometry(uint32_t windowId, int frameW, int frameH,
                                          bool preservePendingTarget) {
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

        // A commit can acknowledge the only in-flight configure while pointer
        // motion has already advanced pending geometry. Keep the resize anchor
        // and newest target until the follow-up configure is committed.
        if (!win.isResizing && !preservePendingTarget) {
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
        if (!win.geometryTransitionActive) {
            win.presentationX = static_cast<float>(finalX);
            win.presentationY = static_cast<float>(finalY);
            win.presentationWidth = static_cast<float>(frameW);
            win.presentationHeight = static_cast<float>(frameH);
        }
        win.pendingX = finalX;
        win.pendingY = finalY;
        if (!win.isResizing && !preservePendingTarget) {
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

void WindowManager::setInsetBorderEnabled(uint32_t windowId, bool enabled) {
    for (auto& win : m_windows) {
        if (win.id == windowId) {
            if (win.drawInsetBorder != enabled) {
                win.drawInsetBorder = enabled;
                win.markDirty();
                m_mouseDirty = true;
            }
            break;
        }
    }
}

void WindowManager::setWindowCornerRadius(uint32_t windowId, float radiusPx) {
    const float clamped = std::max(0.0f, radiusPx);
    for (auto& win : m_windows) {
        if (win.id == windowId) {
            if (std::abs(win.cornerRadiusPx - clamped) > 0.01f) {
                win.cornerRadiusPx = clamped;
                win.markDirty();
                m_mouseDirty = true;
            }
            break;
        }
    }
}

void WindowManager::setReservedZone(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    m_reservedZone = {top, bottom, left, right};
    std::cout << "[LCL WindowManager] Reserved Zone set to top=" << top << " bottom=" << bottom << " left=" << left << " right=" << right << "\n";
}

bool WindowManager::beginWindowDrag(uint32_t windowId, int localX, int localY) {
    focusWindow(windowId);
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end() || it->isUnfocusable || it->isMinimized) return false;

    it->isDragging = true;
    it->isResizing = false;
    it->snapBackActive = false;
    m_motionEngine.clearObjectChannels(it->id);
    it->snapVelX = 0.0f;
    it->snapVelY = 0.0f;
    it->dragOffsetX = std::clamp(localX, 0, std::max(0, it->width - 1));
    it->dragOffsetY = std::clamp(localY, 0, std::max(0, it->height - 1));
    it->markDirty();
    m_mouseDirty = true;
    return true;
}

bool WindowManager::minimizeWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end() || it->isUnfocusable || it->isMinimized) return false;

    const bool wasFocused = it->isFocused;
    it->isMinimized = true;
    it->isDragging = false;
    it->isResizing = false;
    it->snapBackActive = false;
    it->markDirty();
    if (wasFocused) {
        unfocusAll();
        focusTopmostVisibleWindow();
    }
    m_mouseDirty = true;
    return true;
}

bool WindowManager::maximizeWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end() || it->isUnfocusable || it->isMaximized) return false;

    it->isMinimized = false;
    it->restoreX = it->x;
    it->restoreY = it->y;
    it->restoreWidth = it->width;
    it->restoreHeight = it->height;
    startGeometryTransition(*it,
        static_cast<int>(m_reservedZone.left),
        static_cast<int>(m_reservedZone.top),
        std::max(1, static_cast<int>(m_screenWidth) - static_cast<int>(m_reservedZone.left) - static_cast<int>(m_reservedZone.right)),
        std::max(1, static_cast<int>(m_screenHeight) - static_cast<int>(m_reservedZone.top) - static_cast<int>(m_reservedZone.bottom)));
    it->isMaximized = true;
    it->isDragging = false;
    it->isResizing = false;
    it->activeResizeEdge = ResizeEdge::None;
    it->markDirty();
    focusWindow(windowId);
    m_mouseDirty = true;
    return true;
}

bool WindowManager::restoreWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end() || it->isUnfocusable) return false;

    const bool wasMinimized = it->isMinimized;
    const bool wasMaximized = it->isMaximized;
    if (!wasMinimized && !wasMaximized) return false;

    it->isMinimized = false;
    // Restoring a minimized maximized window returns it to its still-maximized
    // work-area geometry. Only an explicit restore from visible maximized state
    // returns to the saved pre-maximize bounds.
    if (wasMaximized && !wasMinimized) {
        startGeometryTransition(*it, it->restoreX, it->restoreY,
                                std::max(1, it->restoreWidth),
                                std::max(1, it->restoreHeight));
        it->isMaximized = false;
    }
    it->markDirty();
    focusWindow(windowId);
    m_mouseDirty = true;
    return true;
}

bool WindowManager::toggleMaximizeWindow(uint32_t windowId) {
    const auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });
    if (it == m_windows.end()) return false;
    return it->isMaximized ? restoreWindow(windowId) : maximizeWindow(windowId);
}

void WindowManager::startGeometryTransition(Window& window, int targetX, int targetY,
                                            int targetWidth, int targetHeight) {
    const auto motion = lcl::motion::tokens::windowMorph();
    const auto animate = [&](uint32_t property, float current, float target) {
        const auto channel = m_motionEngine.ensureChannel({window.id, property}, current);
        m_motionEngine.animateTo(channel, target, motion);
    };
    animate(10, window.presentationX, static_cast<float>(targetX));
    animate(11, window.presentationY, static_cast<float>(targetY));
    animate(12, window.presentationWidth, static_cast<float>(targetWidth));
    animate(13, window.presentationHeight, static_cast<float>(targetHeight));
    window.x = window.pendingX = targetX;
    window.y = window.pendingY = targetY;
    window.width = window.pendingWidth = targetWidth;
    window.height = window.pendingHeight = targetHeight;
    window.geometryTransitionActive = true;
    window.markDirty();
}

bool WindowManager::rollbackWindowGeometry(uint32_t windowId, const Rect& geometry,
                                           bool wasMaximized, bool wasMinimized) {
    auto found = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& window) {
        return window.id == windowId;
    });
    if (found == m_windows.end()) return false;
    m_motionEngine.clearObjectChannels(windowId);
    found->x = found->pendingX = geometry.x;
    found->y = found->pendingY = geometry.y;
    found->width = found->pendingWidth = std::max(1, geometry.width);
    found->height = found->pendingHeight = std::max(1, geometry.height);
    found->presentationX = static_cast<float>(found->x);
    found->presentationY = static_cast<float>(found->y);
    found->presentationWidth = static_cast<float>(found->width);
    found->presentationHeight = static_cast<float>(found->height);
    found->presentationInitialized = true;
    found->geometryTransitionActive = false;
    found->isMaximized = wasMaximized;
    found->isMinimized = wasMinimized;
    found->markDirty();
    return true;
}

void WindowManager::focusWindow(uint32_t windowId) {
    auto it = std::find_if(m_windows.begin(), m_windows.end(), [windowId](const Window& w) {
        return w.id == windowId;
    });

    if (it != m_windows.end() && !it->isMinimized) {
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

void WindowManager::focusTopmostVisibleWindow() {
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        if (!it->isMinimized && !it->isUnfocusable) {
            it->isFocused = true;
            it->headerColor = lcl::theme::UI::WindowTitleFocused;
            it->markDirty();
            break;
        }
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
