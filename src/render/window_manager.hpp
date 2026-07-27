#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "core/input/input_manager.hpp"
#include "render/damage_tracker.hpp"

namespace lcl::render {

enum class ResizeEdge {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

struct Window {
    uint32_t id{0};
    std::string title;
    int x{0};
    int y{0};
    int pendingX{0};
    int pendingY{0};
    int width{400};
    int height{300};
    int pendingWidth{400};
    int pendingHeight{300};
    int zIndex{0};
    bool isFocused{false};

    // Drag state
    bool isDragging{false};
    int dragOffsetX{0};
    int dragOffsetY{0};

    // Resize state
    bool isResizing{false};
    ResizeEdge resizeEdge{ResizeEdge::None};
    int resizeStartX{0};
    int resizeStartY{0};
    int initialX{0};
    int initialY{0};
    int initialWidth{0};
    int initialHeight{0};

    uint32_t headerColor{0xFF38BDF8};

    // Damage Tracking & Occlusion Culling
    bool isDirty{true};
    Rect damageRect{0, 0, 400, 300};

    Rect getBounds() const {
        return Rect{x, y, width, height};
    }

    void markDirty() {
        isDirty = true;
        damageRect = Rect{x, y, width, height};
    }

    void markDirty(const Rect& rect) {
        if (!isDirty) {
            isDirty = true;
            damageRect = rect;
        } else {
            damageRect = Rect::Union(damageRect, rect);
        }
    }

    void clearDirty() {
        isDirty = false;
        damageRect = Rect{0, 0, 0, 0};
    }
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
     * @return True if window state or mouse position changed requiring redraw.
     */
    bool processInputEvent(const core::InputEvent& ev);

    /**
     * @brief Commit attached client surface geometry and calculate position shift for anchor preservation.
     * @param windowId Target window ID.
     * @param frameW Total attached surface frame width.
     * @param frameH Total attached surface frame height (including titlebar).
     */
    void commitSurfaceGeometry(uint32_t windowId, int frameW, int frameH);

    /**
     * @brief Focus a window by ID and bring it to top z-order.
     */
    void focusWindow(uint32_t windowId);

    /**
     * @brief Check if any window or cursor state is dirty.
     */
    bool isAnyWindowDirty() const;

    /**
     * @brief Mark all windows dirty (full redraw request).
     */
    void markAllDirty();

    /**
     * @brief Clear dirty flags across all windows.
     */
    void clearAllDirty();

    uint32_t getFocusedWindowId() const {
        for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
            if (it->isFocused) return it->id;
        }
        return 0;
    }

    const std::vector<Window>& getWindows() const { return m_windows; }
    std::vector<Window>& getWindowsMutable() { return m_windows; }
    int getMouseX() const { return m_mouseX; }
    int getMouseY() const { return m_mouseY; }

private:
    void unfocusAll(); ///< Clear focus + reset header color on all windows
    void updateWindowZOrders();

    uint32_t m_screenWidth{1024};
    uint32_t m_screenHeight{768};
    std::vector<Window> m_windows;
    int m_mouseX{512};
    int m_mouseY{384};
    uint32_t m_nextWindowId{1};
    bool m_initialized{false};
    bool m_mouseDirty{true};
};

} // namespace lcl::render

