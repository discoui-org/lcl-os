#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include "core/input/input_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "render/damage_tracker.hpp"
#include "render/window_chrome_widget.hpp"
#include "lcl-motion/motion.hpp"

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

enum class DecorationMode {
    SSD,  // Server-Side Decoration
    CSD,  // Client-Side Decoration
    None  // Frameless / No Decoration
};

enum class GeometryPhase {
    Idle,
    Morph,
    LiveTransition,
    Drag,
    Resize,
    SnapBack,
};

struct PresentedBounds {
    float x{0.0f};
    float y{0.0f};
    float width{1.0f};
    float height{1.0f};
};

struct GeometryInteraction {
    uint32_t windowId{0};
    uint64_t generation{0};

    operator bool() const noexcept { return windowId != 0 && generation != 0; }
};

struct WindowInputResult {
    bool stateChanged{false};
    GeometryInteraction interaction{};

    operator bool() const noexcept { return stateChanged; }
};

struct ReservedZone {
    uint32_t top{0};
    uint32_t bottom{0};
    uint32_t left{0};
    uint32_t right{0};
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
    float presentationX{0.0f};
    float presentationY{0.0f};
    float presentationWidth{400.0f};
    float presentationHeight{300.0f};
    bool presentationInitialized{false};
    GeometryPhase geometryPhase{GeometryPhase::Idle};
    uint64_t geometryGeneration{1};
    // Live resize animates configure bounds and waits for matching client
    // buffers. Unlike a compositor morph, the currently displayed buffer is
    // never scaled to the interpolated geometry.
    bool liveResizeMotionFinished{false};
    int liveResizeTargetX{0};
    int liveResizeTargetY{0};
    int liveResizeTargetWidth{0};
    int liveResizeTargetHeight{0};
    int zIndex{0};
    bool isFocused{false};
    bool isUnfocusable{false};
    DecorationMode decorationMode{DecorationMode::SSD};
    protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};

    // Drag state
    int dragOffsetX{0};
    int dragOffsetY{0};
    float lastDragVelX{0.0f};
    float lastDragVelY{0.0f};

    float snapX{0.0f};
    float snapY{0.0f};
    float snapVelX{0.0f};
    float snapVelY{0.0f};
    float snapTargetX{0.0f};
    float snapTargetY{0.0f};

    // Resize state
    ResizeEdge resizeEdge{ResizeEdge::None};
    ResizeEdge activeResizeEdge{ResizeEdge::None};
    int resizeStartX{0};
    int resizeStartY{0};
    int initialX{0};
    int initialY{0};
    int initialWidth{0};
    int initialHeight{0};
    int anchorRight{0};
    int anchorBottom{0};

    bool closeRequested{false};
    bool isMinimized{false};
    bool isMaximized{false};
    int restoreX{0};
    int restoreY{0};
    int restoreWidth{0};
    int restoreHeight{0};
    bool drawInsetBorder{true};
    float cornerRadiusPx{-1.0f}; // < 0 means use compositor default policy
    float cornerRoundness{2.0f};
    protocol::LCLResizePresentationMode resizePresentation{
        protocol::LCLResizePresentationMode::CompositorMorph};

    uint32_t headerColor{0xFF38BDF8};
    // Window is the parent presentation group. The compositor-owned titlebar
    // widget and the attached client surface are its two children.
    WindowChromeWidget chrome{};

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

    bool isDragging() const noexcept { return geometryPhase == GeometryPhase::Drag; }
    bool isResizing() const noexcept { return geometryPhase == GeometryPhase::Resize; }
    bool isMorphing() const noexcept { return geometryPhase == GeometryPhase::Morph; }
    bool isLiveTransitioning() const noexcept {
        return geometryPhase == GeometryPhase::LiveTransition;
    }
    bool isSnappingBack() const noexcept { return geometryPhase == GeometryPhase::SnapBack; }
};

inline PresentedBounds presentedBounds(const Window& window) noexcept {
    if (!window.presentationInitialized) {
        return {static_cast<float>(window.x), static_cast<float>(window.y),
                static_cast<float>(window.width), static_cast<float>(window.height)};
    }
    return {window.presentationX, window.presentationY,
            window.presentationWidth, window.presentationHeight};
}

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
    uint32_t getScreenWidth() const noexcept { return m_screenWidth; }
    uint32_t getScreenHeight() const noexcept { return m_screenHeight; }

    /**
     * @brief Create a new window dynamically.
     */
    uint32_t createWindow(const std::string& title, int x, int y, int width, int height,
                          uint32_t headerColor = 0xFF38BDF8, bool focus = true);

    /**
     * @brief Remove / close a window by ID.
     */
    bool removeWindow(uint32_t windowId);

    /**
     * @brief Process input event for hit testing, window focus, and dragging.
     * @return Redraw state plus the generation of any newly authoritative interaction.
     */
    WindowInputResult processInputEvent(const core::InputEvent& ev);

    /**
     * @brief Advance spring snap-back animations for dragged windows.
     * @return True if any window geometry changed.
     */
    bool updateAnimations();
    bool updateAnimations(float dtSec);

    /**
     * @brief Commit attached client surface geometry and calculate position shift for anchor preservation.
     * @param windowId Target window ID.
     * @param frameW Total attached surface frame width.
     * @param frameH Total attached surface frame height (including titlebar).
     * @param expectedGeneration Zero for initial/legacy commits, otherwise the
     * generation that issued the accepted configure.
     */
    bool commitSurfaceGeometry(uint32_t windowId, int frameW, int frameH,
                               bool preservePendingTarget = false,
                               int configuredX = 0, int configuredY = 0,
                               uint64_t expectedGeneration = 0);

    /**
     * @brief Set decoration mode (SSD/CSD/None) for a window.
     */
    void setDecorationMode(uint32_t windowId, DecorationMode mode);

    /**
     * @brief Set window layer (Bottom/Normal/TopMost) and unfocusable flag for a window.
     */
    void setWindowLayer(uint32_t windowId, protocol::LCLWindowLayer layer, bool unfocusable = false);

    /**
     * @brief Enable or disable compositor-forced inset border for a window.
     */
    void setInsetBorderEnabled(uint32_t windowId, bool enabled);

    /**
     * @brief Set compositor mask corner radius for a window in pixels.
     */
    void setWindowCornerRadius(uint32_t windowId, float radiusPx);
    /** Set compositor mask radius and superellipse exponent as one WindowGroup style. */
    void setWindowCornerStyle(uint32_t windowId, float radiusPx, float roundness);
    void setResizePresentationMode(uint32_t windowId,
                                   protocol::LCLResizePresentationMode mode);

    /**
     * @brief Set reserved desktop struts (No Window Move Zone for Menu Bar / Dock).
     */
    void setReservedZone(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);

    /** Start a compositor-owned drag from a client-local pointer position. */
    GeometryInteraction beginWindowDrag(uint32_t windowId, int localX, int localY);
    bool minimizeWindow(uint32_t windowId);
    bool maximizeWindow(uint32_t windowId, bool animateGeometry = true);
    bool restoreWindow(uint32_t windowId, bool animateGeometry = true);
    bool toggleMaximizeWindow(uint32_t windowId, bool animateGeometry = true);
    bool rollbackWindowGeometry(uint32_t windowId, const Rect& geometry,
                                bool wasMaximized, bool wasMinimized,
                                uint64_t expectedGeneration = 0);

    const ReservedZone& getReservedZone() const { return m_reservedZone; }

    /**
     * @brief Focus a window by ID and bring it to top z-order within its layer.
     */
    void focusWindow(uint32_t windowId);

    /**
     * @brief Sort windows by layer (Bottom -> Normal -> TopMost) preserving relative Z-order.
     */
    void sortWindowsByLayer();

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
            if (it->isFocused && !it->isMinimized) return it->id;
        }
        return 0;
    }

    const std::vector<Window>& getWindows() const { return m_windows; }
    std::vector<Window>& getWindowsMutable() { return m_windows; }
    int getMouseX() const { return m_mouseX; }
    int getMouseY() const { return m_mouseY; }

private:
    void unfocusAll(); ///< Clear focus + reset header color on all windows
    void focusTopmostVisibleWindow();
    void updateWindowZOrders();
    void startGeometryTransition(Window& window, int targetX, int targetY,
                                 int targetWidth, int targetHeight,
                                 bool animate);
    uint64_t beginGeometryInteraction(Window& window, GeometryPhase phase);
    void initializeResizeInteraction(Window& window, ResizeEdge edge);
    void settleGeometry(Window& window, GeometryPhase phase = GeometryPhase::Idle);
    void refreshChromeHoverState();

    uint32_t m_screenWidth{1024};
    uint32_t m_screenHeight{768};
    std::vector<Window> m_windows;
    ReservedZone m_reservedZone{};
    int m_mouseX{512};
    int m_mouseY{384};
    double m_subpixelX{512.0};
    double m_subpixelY{384.0};
    uint32_t m_nextWindowId{1};
    bool m_initialized{false};
    bool m_mouseDirty{true};
    std::chrono::steady_clock::time_point m_lastAnimTick{};
    lcl::motion::AnimationEngine m_motionEngine;
};

} // namespace lcl::render
