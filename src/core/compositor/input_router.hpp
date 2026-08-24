#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <utility>

#include "core/compositor/surface_registry.hpp"
#include "core/input/input_manager.hpp"
#include "core/input/system_gesture_arena.hpp"
#include "core/scene/scene_registry.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Routes physical input through the window manager and into the focused client surface. */
class InputRouter {
public:
    InputRouter(render::WindowManager& windowManager,
                SurfaceRegistry& surfaces,
                SceneRegistry& scenes,
                float outputScale = 1.0f,
                bool systemGesturesEnabled = false,
                bool desktopWindowManagementEnabled = true)
        : m_windowManager(windowManager), m_surfaces(surfaces), m_scenes(scenes),
          m_outputScale(outputScale),
          m_systemGesturesEnabled(systemGesturesEnabled),
          m_desktopWindowManagementEnabled(desktopWindowManagementEnabled) {}

    using SystemGestureHandler = std::function<bool(
        SystemGestureDecision, const SystemGestureProgress&)>;
    void setSystemGestureHandler(SystemGestureHandler handler) {
        m_systemGestureHandler = std::move(handler);
    }

    /**
     * Applies compositor interaction (hit test/focus/resize) then sends a
     * logical-coordinate protocol event to the focused client when applicable.
     * Returns true when a new compositor frame is required.
     */
    bool route(const InputEvent& event);

    /** Send configure events after a compositor-originated window action. */
    void syncWindowState();
    void setRefreshInterval(std::chrono::nanoseconds interval) {
        if (interval.count() > 0) m_refreshInterval = interval;
    }

private:
    void sendPendingConfigures();
    void processCloseRequests();
    void forwardToFocusedSurface(const InputEvent& event) const;
    void forwardToSurface(const InputEvent& event,
                          SurfaceRegistry::Key surfaceKey) const;
    SurfaceRegistry::Key focusedSurfaceKey() const;
    SurfaceRegistry::Key findPopupAt(float globalX, float globalY) const;
    void destroyPopupChildren(SurfaceRegistry::Key parentSurfaceKey);

    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    SceneRegistry& m_scenes;
    float m_outputScale{1.0f};
    bool m_systemGesturesEnabled{false};
    bool m_desktopWindowManagementEnabled{true};
    SystemGestureArena m_systemGestureArena;
    SystemGestureHandler m_systemGestureHandler;
    std::unordered_map<uint32_t, SurfaceRegistry::Key> m_touchTargets;
    SurfaceRegistry::Key m_activePopupSurface{0};
    std::chrono::nanoseconds m_refreshInterval{std::chrono::nanoseconds(16666667)};
};

} // namespace lcl::core
