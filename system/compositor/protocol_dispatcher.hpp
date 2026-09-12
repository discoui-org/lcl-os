#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

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
#include "lcl-motion/motion.hpp"

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
    ~ProtocolDispatcher();

    /** Process every queued IPC message and report whether a frame is required. */
    bool process(IPCManager& ipcManager);
    /** Import immutable storage; visibility remains pending until its manifest. */
    bool acceptRasterLayer(RasterServiceHost::ReceivedLayer layer);
    /** Atomically promote a complete imported retained-presentation frame. */
    bool acceptPresentationFrame(
        RasterServiceHost::ReceivedPresentationFrame frame);
    /**
     * Retry manifests that arrived before their side-band/native storage.
     * A retry never exposes a partial snapshot; it only promotes once every
     * declared layer has been imported.
     */
    bool retryPendingPresentationFrames();
    void acceptPresentationAnimation(
        RasterServiceHost::ReceivedPresentationAnimation animation);
    /** Sample compositor-owned presentation motion once per compositor tick. */
    bool advancePresentationAnimations(float deltaSeconds);
    /** Revoke a surface grant and every invisible imported layer it owns. */
    void releaseSurfaceRasterState(SurfaceRegistry::SurfaceEntry& entry);

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
    struct PendingRasterLayer {
        SurfaceRegistry::Key surfaceKey{0};
        raster_protocol::LayerReady metadata{};
        int shmFd{-1};
        void* pixels{nullptr};
        size_t shmSize{0};
        uint32_t texture{0};
    };
    void releasePendingRasterLayer(
        uint64_t layerId, raster_protocol::LayerReleaseReason reason);
    void releasePendingRasterLayersForSurface(
        SurfaceRegistry::Key surfaceKey,
        raster_protocol::LayerReleaseReason reason);
    bool acceptPresentationFrameImpl(
        RasterServiceHost::ReceivedPresentationFrame frame,
        bool deferIncomplete);
    void deferPresentationFrame(
        RasterServiceHost::ReceivedPresentationFrame frame);
    void discardPendingPresentationFramesForSurface(
        SurfaceRegistry::Key surfaceKey);
    bool promoteRootPresentation(
        SurfaceRegistry::iterator surface,
        const raster_protocol::PresentationFrameReady& frame,
        const raster_protocol::PresentationLayerState& rootLayer);
    struct PresentationAnimationKey {
        SurfaceRegistry::Key surfaceKey{0};
        uint64_t nodeId{0};
        bool operator==(const PresentationAnimationKey&) const = default;
    };
    struct PresentationAnimationKeyHash {
        size_t operator()(const PresentationAnimationKey& key) const noexcept {
            return std::hash<uint64_t>{}(key.surfaceKey) ^
                (std::hash<uint64_t>{}(key.nodeId) + 0x9e3779b97f4a7c15ull +
                 (key.surfaceKey << 6u) + (key.surfaceKey >> 2u));
        }
    };
    struct ActivePresentationAnimation {
        raster_protocol::PresentationAnimation declaration{};
        uint64_t layerId{0};
        uint64_t contentRevision{0};
        lcl::motion::ChannelId translationX{0};
        lcl::motion::ChannelId translationY{0};
        lcl::motion::ChannelId scaleX{0};
        lcl::motion::ChannelId scaleY{0};
        lcl::motion::ChannelId rotation{0};
        lcl::motion::ChannelId opacity{0};
    };
    static uint64_t presentationAnimationObjectId(
        SurfaceRegistry::Key surfaceKey, uint64_t nodeId) noexcept;
    bool sendPresentationAnimationResult(
        const raster_protocol::PresentationAnimation& animation,
        raster_protocol::PresentationAnimationOutcome outcome);

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
    std::unordered_map<uint64_t, PendingRasterLayer> m_pendingRasterLayers;
    // The main private socket and Android's native-buffer socket are
    // independent. A manifest can therefore arrive one compositor poll before
    // its AHardwareBuffer; retain its metadata rather than rejecting the
    // complete snapshot or showing only the root layer.
    std::vector<RasterServiceHost::ReceivedPresentationFrame>
        m_pendingPresentationFrames;
    lcl::motion::AnimationEngine m_presentationMotion;
    std::unordered_map<PresentationAnimationKey, ActivePresentationAnimation,
                       PresentationAnimationKeyHash>
        m_activePresentationAnimations;
    std::unordered_map<int, protocol::LCLSystemSurfaceKind> m_pendingSystemSurfaceKinds;
    struct ShellSubscription {
        uint64_t revision{0};
        bool hasDeliveredState{false};
    };
    std::unordered_map<int, ShellSubscription> m_shellSubscriptions;
};

} // namespace lcl::core
