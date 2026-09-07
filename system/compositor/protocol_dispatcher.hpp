#pragma once

#include <cstdint>
#include <unordered_map>

#include "system/compositor/surface_registry.hpp"
#include "system/ipc/ipc_manager.hpp"
#include "system/scene/focus_controller.hpp"
#include "system/scene/scene_registry.hpp"
#include "system/scene/shell_state_broker.hpp"
#include "system/compositor/system_surface_policy.hpp"
#include "system/compositor/windowing_policy.hpp"
#include "system/compositor/raster_service_host.hpp"
#include "system/security/application_peer_authenticator.hpp"
#include "system/render/renderer.hpp"
#include "system/render/window_manager.hpp"

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
                       const WindowingPolicy& windowingPolicy,
                       RasterServiceHost& rasterService,
                       const security::ApplicationPeerAuthenticator& peerAuthenticator)
        : m_renderer(renderer), m_windowManager(windowManager), m_surfaces(surfaces),
          m_scenes(scenes), m_focus(focus), m_shellState(shellState),
          m_windowingPolicy(windowingPolicy), m_rasterService(rasterService),
          m_peerAuthenticator(peerAuthenticator) {}
    ~ProtocolDispatcher() = default;

    /** Process every queued IPC message and report whether a frame is required. */
    bool process(IPCManager& ipcManager);
    bool acceptRasterLayer(RasterServiceHost::ReceivedLayer layer);

    /** Publish revisioned scene/focus state to typed shell subscribers. */
    void publishShellStateToSubscribers();

    /** Notify the HomeScreen that its stationary launch icon may be shown. */
    bool publishLaunchIconVisibility(const SurfaceRegistry::SurfaceEntry& entry,
                                     bool visible) const;
    /** Commit a complete WindowGroup's configured parent geometry. */
    bool commitAtomicSurfaceGeometry(uint32_t windowId,
                                     uint64_t generation);

private:
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    using SurfaceEffectRegion = SurfaceRegistry::SurfaceEffectRegion;
    void recomputeSystemReservedZone();
    bool commitClientSurfaceGeometry(SurfaceEntry& entry);
    bool mapSurface(SurfaceRegistry::Key surfaceKey, SurfaceEntry& entry,
                    pid_t clientPid);
    void grantRasterSurface(SurfaceEntry& entry, uint32_t surfaceId,
                            pid_t clientPid, bool interactiveSystem);
    void revokeRasterSurface(SurfaceEntry& entry);

    render::Renderer& m_renderer;
    render::WindowManager& m_windowManager;
    SurfaceRegistry& m_surfaces;
    SceneRegistry& m_scenes;
    FocusController& m_focus;
    ShellStateBroker& m_shellState;
    const WindowingPolicy& m_windowingPolicy;
    RasterServiceHost& m_rasterService;
    const security::ApplicationPeerAuthenticator& m_peerAuthenticator;
    uint64_t m_nextShmContentSerial{1};
    std::unordered_map<int, protocol::LCLSystemSurfaceKind> m_pendingSystemSurfaceKinds;
    struct ShellSubscription {
        uint64_t revision{0};
        bool hasDeliveredState{false};
    };
    std::unordered_map<int, ShellSubscription> m_shellSubscriptions;
};

} // namespace lcl::core
