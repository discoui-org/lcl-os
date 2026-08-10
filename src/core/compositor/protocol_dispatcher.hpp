#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/compositor/surface_registry.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Validates and dispatches compositor IPC commands; it owns client roles. */
class ProtocolDispatcher {
public:
    ProtocolDispatcher(render::Renderer& renderer,
                       render::WindowManager& windowManager,
                       SurfaceRegistry& surfaces)
        : m_renderer(renderer), m_windowManager(windowManager), m_surfaces(surfaces) {}

    /** Process every queued IPC message and report whether a frame is required. */
    bool process(IPCManager& ipcManager);

    /** Publish the shell window list only when its observable content changed. */
    void publishWindowListToShellClients();

private:
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    using SurfaceEffectRegion = SurfaceRegistry::SurfaceEffectRegion;

    render::Renderer& m_renderer;
    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    std::unordered_map<int, protocol::LCLRole> m_clientRoles;
    uint64_t m_lastWindowListHash{0};
};

} // namespace lcl::core
