#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/compositor/surface_registry.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/scene/scene_registry.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Validates and dispatches compositor IPC commands; it owns client roles. */
class ProtocolDispatcher {
public:
    ProtocolDispatcher(render::Renderer& renderer,
                       render::WindowManager& windowManager,
                       SurfaceRegistry& surfaces,
                       SceneRegistry& scenes)
        : m_renderer(renderer), m_windowManager(windowManager), m_surfaces(surfaces), m_scenes(scenes) {}

    /** Process every queued IPC message and report whether a frame is required. */
    bool process(IPCManager& ipcManager);

    /** Publish each shell client's window list when its observable content changed. */
    void publishWindowListToShellClients();

private:
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    using SurfaceEffectRegion = SurfaceRegistry::SurfaceEffectRegion;

    render::Renderer& m_renderer;
    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    SceneRegistry& m_scenes;
    std::unordered_map<int, protocol::LCLRole> m_clientRoles;
    // Shell surfaces may be recreated during a display reconfigure.  Snapshot
    // suppression is therefore per receiver: a fresh dock must receive the
    // current list even when no application window changed in the meantime.
    std::unordered_map<int, uint64_t> m_lastWindowListHashes;
};

} // namespace lcl::core
