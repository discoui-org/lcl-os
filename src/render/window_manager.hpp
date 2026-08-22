#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include "core/input/input_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "render/damage_tracker.hpp"
#include "lcl-window-chrome/window_chrome.hpp"
#include "lcl-motion/motion.hpp"
#include "lcl-graphics/geometry.hpp"

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

enum class GeometryInteractionKind {
    None,
    Manual,
    WindowStateTransition,
};

struct GeometryInteraction {
    uint32_t windowId{0};
    uint64_t generation{0};
    GeometryInteractionKind kind{GeometryInteractionKind::None};
    graphics::RectF previousBounds{};
    bool previousWasMaximized{false};
    bool previousWasMinimized{false};

    operator bool() const noexcept { return windowId != 0 && generation != 0; }
    bool isManual() const noexcept { return kind == GeometryInteractionKind::Manual; }
    bool isWindowStateTransition() const noexcept {
        return kind == GeometryInteractionKind::WindowStateTransition;
    }

    static GeometryInteraction manual(uint32_t windowId, uint64_t generation) noexcept {
        return {windowId, generation, GeometryInteractionKind::Manual};
    }

    static GeometryInteraction windowStateTransition(
            uint32_t windowId, uint64_t generation, const graphics::RectF& previousBounds,
            bool previousWasMaximized, bool previousWasMinimized) noexcept {
        return {windowId, generation, GeometryInteractionKind::WindowStateTransition,
                previousBounds, previousWasMaximized, previousWasMinimized};
    }
};

struct WindowInputResult {
    bool stateChanged{false};
    GeometryInteraction interaction{};

    operator bool() const noexcept { return stateChanged; }
};

struct ReservedZone {
    float top{0.0f};
    float bottom{0.0f};
    float left{0.0f};
    float right{0.0f};
};

struct Window {
    uint32_t id{0};
    std::string title;
    float x{0.0f};
    float y{0.0f};
    float pendingX{0.0f};
    float pendingY{0.0f};
    float width{400.0f};
    float height{300.0f};
    float pendingWidth{400.0f};
    float pendingHeight{300.0f};
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
    float liveResizeTargetX{0.0f};
    float liveResizeTargetY{0.0f};
    float liveResizeTargetWidth{0.0f};
    float liveResizeTargetHeight{0.0f};
    int zIndex{0};
    bool isFocused{false};
    bool isUnfocusable{false};
    DecorationMode decorationMode{DecorationMode::SSD};
    bool edgeToEdge{false};
    protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};

    // Drag state
    float dragOffsetX{0.0f};
    float dragOffsetY{0.0f};
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
    float resizeStartX{0.0f};
    float resizeStartY{0.0f};
    float initialX{0.0f};
    float initialY{0.0f};
    float initialWidth{0.0f};
    float initialHeight{0.0f};
    float anchorRight{0.0f};
    float anchorBottom{0.0f};

    bool closeRequested{false};
    bool isMinimized{false};
    bool isMaximized{false};
    float restoreX{0.0f};
    float restoreY{0.0f};
    float restoreWidth{0.0f};
    float restoreHeight{0.0f};
    bool drawInsetBorder{true};
    float cornerRadius{-1.0f}; // < 0 means use compositor default policy
    float cornerRoundness{2.0f};
    protocol::LCLResizePresentationMode resizePresentation{
        protocol::LCLResizePresentationMode::CompositorMorph};

    uint32_t headerColor{0xFF38BDF8};
    // Window is the parent presentation group. The compositor-owned titlebar
    // widget and the attached client surface are its two children.
    lcl::chrome::WindowChromeWidget chrome{};

    // Damage Tracking & Occlusion Culling
    bool isDirty{true};
    graphics::RectF damageRect{0.0f, 0.0f, 400.0f, 300.0f};

    graphics::RectF getBounds() const {
        return {x, y, width, height};
    }

    void markDirty() {
        isDirty = true;
        damageRect = {x, y, width, height};
    }

    void markDirty(const graphics::RectF& rect) {
        if (!isDirty) {
            isDirty = true;
            damageRect = rect;
        } else {
            damageRect = damageRect.unionWith(rect);
        }
    }

    void clearDirty() {
        isDirty = false;
        damageRect = {};
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
    bool initialize(float screenWidth = 1024.0f, float screenHeight = 768.0f);
    float getScreenWidth() const noexcept { return m_screenWidth; }
    float getScreenHeight() const noexcept { return m_screenHeight; }

    /**
     * @brief Create a new window dynamically.
     */
    uint32_t createWindow(const std::string& title, float x, float y, float width, float height,
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
    bool commitSurfaceGeometry(uint32_t windowId, float frameW, float frameH,
                               bool preservePendingTarget = false,
                               float configuredX = 0.0f, float configuredY = 0.0f,
                               uint64_t expectedGeneration = 0);

    /**
     * @brief Set decoration mode (SSD/CSD/None) for a window.
     */
    void setDecorationMode(uint32_t windowId, DecorationMode mode);
    /** Extend surface material beneath compositor-owned system insets. */
    void setEdgeToEdge(uint32_t windowId, bool enabled);

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
    void setWindowCornerRadius(uint32_t windowId, float radius);
    /** Set compositor mask radius and superellipse exponent as one WindowGroup style. */
    void setWindowCornerStyle(uint32_t windowId, float radius, float roundness);
    void setResizePresentationMode(uint32_t windowId,
                                   protocol::LCLResizePresentationMode mode);

    /**
     * @brief Set reserved desktop struts (No Window Move Zone for Menu Bar / Dock).
     */
    void setReservedZone(float top, float bottom, float left, float right);

    /** Start a compositor-owned drag from a client-local pointer position. */
    GeometryInteraction beginWindowDrag(uint32_t windowId, float localX, float localY);
    bool minimizeWindow(uint32_t windowId);
    bool maximizeWindow(uint32_t windowId, bool animateGeometry = true);
    bool restoreWindow(uint32_t windowId, bool animateGeometry = true);
    bool toggleMaximizeWindow(uint32_t windowId, bool animateGeometry = true);
    bool rollbackWindowGeometry(uint32_t windowId, const graphics::RectF& geometry,
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
    float getMouseX() const { return m_mouseX; }
    float getMouseY() const { return m_mouseY; }

private:
    void unfocusAll(); ///< Clear focus + reset header color on all windows
    void focusTopmostVisibleWindow();
    void updateWindowZOrders();
    void startGeometryTransition(Window& window, float targetX, float targetY,
                                 float targetWidth, float targetHeight,
                                 bool animate);
    uint64_t beginGeometryInteraction(Window& window, GeometryPhase phase);
    void initializeResizeInteraction(Window& window, ResizeEdge edge);
    void settleGeometry(Window& window, GeometryPhase phase = GeometryPhase::Idle);
    void refreshChromeHoverState();

    float m_screenWidth{1024.0f};
    float m_screenHeight{768.0f};
    std::vector<Window> m_windows;
    ReservedZone m_reservedZone{};
    float m_mouseX{512.0f};
    float m_mouseY{384.0f};
    double m_subpixelX{512.0};
    double m_subpixelY{384.0};
    uint32_t m_nextWindowId{1};
    bool m_initialized{false};
    bool m_mouseDirty{true};
    std::chrono::steady_clock::time_point m_lastAnimTick{};
    lcl::motion::AnimationEngine m_motionEngine;
};

} // namespace lcl::render
