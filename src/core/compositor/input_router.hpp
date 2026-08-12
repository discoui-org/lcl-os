#pragma once

#include "core/compositor/surface_registry.hpp"
#include "core/input/input_manager.hpp"
#include "core/scene/scene_registry.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Routes physical input through the window manager and into the focused client surface. */
class InputRouter {
public:
    InputRouter(render::WindowManager& windowManager,
                SurfaceRegistry& surfaces,
                SceneRegistry& scenes)
        : m_windowManager(windowManager), m_surfaces(surfaces), m_scenes(scenes) {}

    /**
     * Applies compositor interaction (hit test/focus/resize) then sends a
     * logical-coordinate protocol event to the focused client when applicable.
     * Returns true when a new compositor frame is required.
     */
    bool route(const InputEvent& event);

    /** Send configure events after a compositor-originated window action. */
    void syncWindowState();

private:
    void sendPendingConfigures();
    void processCloseRequests();
    void forwardToFocusedSurface(const InputEvent& event) const;
    static bool startClosingTransition(SurfaceRegistry::SurfaceEntry& entry) noexcept;

    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    SceneRegistry& m_scenes;
};

} // namespace lcl::core
