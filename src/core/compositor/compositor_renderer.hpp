#pragma once

#include "core/compositor/surface_registry.hpp"
#include "platform/common/display_backend.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

#include <functional>

namespace lcl::core {

/** Draws and presents one immutable surface snapshot through the compositor backend. */
class CompositorRenderer {
public:
    void render(render::Renderer& renderer,
                lcl::platform::IDisplayBackend& displayBackend,
                const render::WindowManager& windowManager,
                const SurfaceRegistry::Snapshot& surfaces,
                const std::function<void()>& beforePresent = {},
                bool allowIncrementalMove = false,
                bool useMobilePresentation = false) const;

private:
    // Android's compositor scene FBO is authoritative across frames. Partial
    // move damage is enabled only after one complete frame initialized it.
    mutable bool m_hasCompleteRetainedFrame{false};
};

} // namespace lcl::core
