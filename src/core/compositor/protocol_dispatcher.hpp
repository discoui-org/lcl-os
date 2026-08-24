#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/compositor/surface_registry.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/scene/focus_controller.hpp"
#include "core/scene/scene_registry.hpp"
#include "core/scene/shell_state_broker.hpp"
#include "core/compositor/system_surface_policy.hpp"
#include "core/compositor/windowing_policy.hpp"
#include "render/renderer.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

/** Validates and dispatches compositor IPC commands and trusted shell capabilities. */
class ProtocolDispatcher {
public:
    ProtocolDispatcher(render::Renderer& renderer,
                       render::WindowManager& windowManager,
                       SurfaceRegistry& surfaces,
                       SceneRegistry& scenes,
                       FocusController& focus,
                       ShellStateBroker& shellState,
                       const WindowingPolicy& windowingPolicy)
        : m_renderer(renderer), m_windowManager(windowManager), m_surfaces(surfaces),
          m_scenes(scenes), m_focus(focus), m_shellState(shellState),
          m_windowingPolicy(windowingPolicy) {}
    ~ProtocolDispatcher();

    /** Process every queued IPC message and report whether a frame is required. */
    bool process(IPCManager& ipcManager);

    /** Publish revisioned scene/focus state to typed shell subscribers. */
    void publishShellStateToSubscribers();

    /** Notify the HomeScreen that its stationary launch icon may be shown. */
    bool publishLaunchIconVisibility(const SurfaceRegistry::SurfaceEntry& entry,
                                     bool visible) const;

    /** Synchronize launcher background presentation with an app icon morph. */
    bool publishLaunchHomeTransition(
        const SurfaceRegistry::SurfaceEntry& entry, float progress) const;

private:
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    using SurfaceEffectRegion = SurfaceRegistry::SurfaceEffectRegion;
    void recomputeSystemReservedZone();

    render::Renderer& m_renderer;
    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    SceneRegistry& m_scenes;
    FocusController& m_focus;
    ShellStateBroker& m_shellState;
    const WindowingPolicy& m_windowingPolicy;
    uint64_t m_nextShmContentSerial{1};
    std::unordered_map<int, protocol::LCLSystemSurfaceKind> m_pendingSystemSurfaceKinds;
    std::unordered_map<int, int> m_nativeBufferChannels;
    struct ShellSubscription {
        uint64_t revision{0};
        bool hasDeliveredState{false};
    };
    std::unordered_map<int, ShellSubscription> m_shellSubscriptions;
};

} // namespace lcl::core
