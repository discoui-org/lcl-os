#pragma once

#include "core/compositor/surface_registry.hpp"
#include "core/display/display_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Draws and presents one immutable surface snapshot through the compositor backend. */
class CompositorRenderer {
public:
    void render(render::Renderer& renderer,
                DisplayManager& displayManager,
                const render::WindowManager& windowManager,
                const SurfaceRegistry::Snapshot& surfaces) const;
};

} // namespace lcl::core
