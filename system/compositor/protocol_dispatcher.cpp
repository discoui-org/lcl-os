#include "system/compositor/protocol_dispatcher.hpp"
#include "system/compositor/effect_region_geometry.hpp"
#include "system/compositor/layer_feedback_handler.hpp"
#include "lcl-motion/motion.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lcl::core {
namespace {
protocol::LCLMsgShellScene toWireScene(const SceneRecord& scene) {
    protocol::LCLMsgShellScene wire{};
    wire.sceneId = scene.id;
    wire.appInstanceId = scene.appInstanceId;
    wire.windowId = scene.windowId;
    wire.clientPid = static_cast<int32_t>(scene.clientPid);
    wire.displayId = scene.displayId;
    wire.workspaceId = scene.workspaceId;
    wire.x = scene.x;
    wire.y = scene.y;
    wire.width = scene.width;
    wire.height = scene.height;
    switch (scene.visibility) {
        case SceneVisibility::Minimized:
            wire.visibility = protocol::LCLSceneVisibility::Minimized;
            break;
        case SceneVisibility::Closing:
            wire.visibility = protocol::LCLSceneVisibility::Closing;
            break;
        case SceneVisibility::Visible:
        default:
            wire.visibility = protocol::LCLSceneVisibility::Visible;
            break;
    }
    wire.decorationMode = scene.decorationMode;
    wire.edgeToEdge = scene.edgeToEdge ? 1 : 0;
    std::strncpy(wire.appId, scene.appId.c_str(), sizeof(wire.appId) - 1);
    std::strncpy(wire.title, scene.title.c_str(), sizeof(wire.title) - 1);
    return wire;
}

protocol::LCLMsgShellStateDelta toWireDelta(const ShellStateDelta& delta) {
    protocol::LCLMsgShellStateDelta wire{};
    wire.revision = delta.revision;
    switch (delta.kind) {
        case ShellStateDelta::Kind::SceneAdded:
            wire.kind = protocol::LCLShellStateDeltaKind::SceneAdded;
            break;
        case ShellStateDelta::Kind::SceneRemoved:
            wire.kind = protocol::LCLShellStateDeltaKind::SceneRemoved;
            break;
        case ShellStateDelta::Kind::FocusChanged:
            wire.kind = protocol::LCLShellStateDeltaKind::FocusChanged;
            break;
        case ShellStateDelta::Kind::SceneUpdated:
        default:
            wire.kind = protocol::LCLShellStateDeltaKind::SceneUpdated;
            break;
    }
    wire.seatId = delta.focus.seatId;
    wire.displayId = delta.focus.displayId;
    wire.workspaceId = delta.focus.workspaceId;
    wire.activeSceneId = delta.focus.activeSceneId;
    if (delta.kind != ShellStateDelta::Kind::FocusChanged) {
        wire.scene = toWireScene(delta.scene);
    }
    return wire;
}

class RequestAck {
public:
    RequestAck(int clientFd, uint32_t requestId)
        : m_clientFd(clientFd), m_requestId(requestId) {}
    ~RequestAck() {
        if (m_clientFd < 0 || m_requestId == 0) return;
        protocol::LCLMsgAckResponse response{};
        response.status = m_status;
        std::strncpy(response.message, m_message.c_str(), sizeof(response.message) - 1);
        protocol::LCLHeader header{};
        header.opcode = protocol::LCLOpcode::AckResponse;
        header.requestId = m_requestId;
        header.payloadSize = sizeof(response);
        protocol::sendMsgWithFd(m_clientFd, header, &response);
    }
    void error(uint32_t status, const char* message) {
        m_status = status;
        m_message = message;
    }
private:
    int m_clientFd{-1};
    uint32_t m_requestId{0};
    uint32_t m_status{0};
    std::string m_message{"ok"};
};

} // namespace

bool ProtocolDispatcher::publishLaunchIconVisibility(
        const SurfaceRegistry::SurfaceEntry& entry, bool visible) const {
    if (entry.launchOwnerFd < 0 || entry.launchToken == 0 ||
        entry.appId.empty()) return false;

    protocol::LCLMsgLaunchIconVisibility message{};
    message.launchToken = entry.launchToken;
    std::strncpy(message.appId, entry.appId.c_str(),
                 sizeof(message.appId) - 1);
    message.visible = visible ? 1 : 0;
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::LaunchIconVisibility;
    header.payloadSize = sizeof(message);
    return protocol::sendMsgWithFd(entry.launchOwnerFd, header, &message);
}

bool ProtocolDispatcher::commitClientSurfaceGeometry(SurfaceEntry& entry) {
    if (entry.isPopup() || entry.isAttached() || entry.windowId == 0) {
        return false;
    }

    float titleOffset = 0.0f;
    const auto window = std::find_if(
        m_windowManager.getWindows().begin(),
        m_windowManager.getWindows().end(),
        [&entry](const auto& candidate) {
            return candidate.id == entry.windowId;
        });
    if (window == m_windowManager.getWindows().end()) return false;
    if (window->decorationMode == render::DecorationMode::SSD) {
        titleOffset = 32.0f;
    }

    const float committedContentW = entry.configuredWidth;
    const float committedContentH = entry.configuredHeight;
    const float frameHeight = committedContentH + titleOffset;
    const bool preserveNewerTarget =
        window->pendingX != entry.configuredX ||
        window->pendingY != entry.configuredY ||
        window->pendingWidth != committedContentW ||
        window->pendingHeight != frameHeight;

    return m_windowManager.commitSurfaceGeometry(
        entry.windowId, committedContentW, frameHeight,
        preserveNewerTarget, entry.configuredX, entry.configuredY,
        entry.configuredGeometryGeneration);
}

bool ProtocolDispatcher::commitAtomicSurfaceGeometry(
        uint32_t windowId, uint64_t generation) {
    const auto parent = std::find_if(
        m_surfaces.begin(), m_surfaces.end(),
        [windowId, generation](const auto& item) {
            const auto& entry = item.second;
            return !entry.isPopup() && !entry.isAttached() &&
                entry.windowId == windowId &&
                entry.atomicConfigureGeneration == generation &&
                entry.configuredGeometryGeneration == generation;
        });
    return parent != m_surfaces.end() &&
        commitClientSurfaceGeometry(parent->second);
}

bool ProtocolDispatcher::mapSurface(SurfaceRegistry::Key surfaceKey,
                                    SurfaceEntry& entry, pid_t clientPid) {
    auto toRenderDecorationMode = [](protocol::LCLDecorationMode mode) {
        switch (mode) {
            case protocol::LCLDecorationMode::CSD: return render::DecorationMode::CSD;
            case protocol::LCLDecorationMode::None: return render::DecorationMode::None;
            case protocol::LCLDecorationMode::SSD:
            default: return render::DecorationMode::SSD;
        }
    };
    if (entry.isPopup() || !entry.hasRenderableBuffer() ||
        entry.width == 0 || entry.height == 0) {
        return false;
    }
    if (entry.isAttached()) {
        const auto parent = std::find_if(
            m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
            [&entry](const auto& window) {
                return window.id == entry.attachedWindowId;
            });
        if (parent == m_windowManager.getWindows().end()) return false;
        entry.hasCommittedBuffer = true;
        return true;
    }
    if (entry.windowId != 0) {
        if (!entry.hasCommittedBuffer) {
            entry.hasCommittedBuffer = true;
            if (entry.launchPlaceholderActive) {
                entry.launchContentOpacity = 0.0f;
                entry.launchContentFadeElapsedSec = 0.0f;
                entry.launchContentFadeActive = true;
            }
            if (entry.systemSurfaceKind == protocol::LCLSystemSurfaceKind::None) {
                m_scenes.mapClientSurface(
                    surfaceKey, clientPid, entry.windowId, entry.appId,
                    entry.title, entry.appInstanceId, entry.decorationMode);
            }
        }
        return true;
    }

    const auto decorationMode = toRenderDecorationMode(entry.decorationMode);
    const int titleOffset = decorationMode == render::DecorationMode::SSD ? 32 : 0;
    entry.windowId = m_windowManager.createWindow(
        entry.title, entry.initialX, entry.initialY, entry.initialWidth,
        entry.initialHeight + static_cast<float>(titleOffset), !entry.unfocusable);
    m_windowManager.setDecorationMode(entry.windowId, decorationMode);
    (void)m_windowManager.setResizeConstraints(
        entry.windowId,
        {entry.resizeBaseWidth, entry.resizeBaseHeight,
         entry.resizeWidthIncrement, entry.resizeHeightIncrement});
    m_windowManager.setEdgeToEdge(entry.windowId, entry.edgeToEdge);
    m_windowManager.setWindowLayer(entry.windowId, entry.layer, entry.unfocusable);
    m_windowManager.setInsetBorderEnabled(entry.windowId, entry.insetBorderEnabled);
    if (entry.cornerRadius >= 0.0f) {
        m_windowManager.setWindowCornerStyle(
            entry.windowId, entry.cornerRadius, entry.cornerRoundness);
    }
    if (entry.suppressInitialTransition) {
        entry.transitionPhase = SurfaceEntry::TransitionPhase::None;
        entry.transitionOpacity = 1.0f;
        entry.transitionScale = 1.0f;
    } else {
        entry.transitionPhase = SurfaceEntry::TransitionPhase::Entering;
        entry.transitionElapsedSec = 0.0f;
        entry.transitionDurationSec =
            lcl::motion::tokens::windowOpen().tweenParams.durationSec;
        entry.transitionOpacity = 0.0f;
        entry.transitionScale = 0.96f;
    }
    entry.hasCommittedBuffer = true;
    if (entry.systemSurfaceKind == protocol::LCLSystemSurfaceKind::None) {
        m_scenes.mapClientSurface(surfaceKey, clientPid, entry.windowId,
                                  entry.appId, entry.title,
                                  entry.appInstanceId, entry.decorationMode);
    }
    std::cout << "[LCL Compositor] Mapped Window (ID: " << entry.windowId
              << ") after first ready raster layer\n";
    return true;
}

void ProtocolDispatcher::grantRasterSurface(
        SurfaceEntry& entry, uint32_t surfaceId, pid_t clientPid,
        bool interactiveSystem) {
    if (entry.producerGrant.tokenHigh == 0 && entry.producerGrant.tokenLow == 0) {
        entry.producerGrant = m_rasterService.registerSurface(
            surfaceId, clientPid, interactiveSystem);
    }
    protocol::LCLMsgSurfaceProducerGrant message{};
    message.surfaceId = entry.producerGrant.surfaceId;
    message.ownerPid = entry.producerGrant.ownerPid;
    message.flags = entry.producerGrant.flags;
    message.tokenHigh = entry.producerGrant.tokenHigh;
    message.tokenLow = entry.producerGrant.tokenLow;
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::SurfaceProducerGrant;
    header.payloadSize = sizeof(message);
    (void)protocol::sendMsgWithFd(entry.clientFd, header, &message);
}

void ProtocolDispatcher::revokeRasterSurface(SurfaceEntry& entry) {
    if (entry.producerGrant.tokenHigh == 0 && entry.producerGrant.tokenLow == 0) {
        return;
    }
    m_rasterService.revokeSurface(entry.producerGrant);
    entry.producerGrant = {};
}

bool ProtocolDispatcher::acceptRasterLayer(RasterServiceHost::ReceivedLayer layer) {
    const auto closeLayer = [&](raster_protocol::LayerReleaseReason reason) {
        if (layer.fd >= 0) close(layer.fd);
        layer.fd = -1;
        m_rasterService.releaseLayer(layer.metadata.layerId, reason);
    };
    const auto& ready = layer.metadata;
    auto surface = std::find_if(
        m_surfaces.begin(), m_surfaces.end(), [&ready](const auto& item) {
            const auto& grant = item.second.producerGrant;
            return grant.surfaceId == ready.grant.surfaceId &&
                grant.ownerPid == ready.grant.ownerPid &&
                grant.tokenHigh == ready.grant.tokenHigh &&
                grant.tokenLow == ready.grant.tokenLow;
        });
    if (surface == m_surfaces.end()) {
        closeLayer(raster_protocol::LayerReleaseReason::SurfaceRevoked);
        return false;
    }
    auto& entry = surface->second;
    const bool shmLayer =
        ready.transport == raster_protocol::LayerTransport::Shm;
    const bool dmaBufLayer =
        ready.transport == raster_protocol::LayerTransport::DmaBuf;
    const bool nativeBufferLayer = ready.transport ==
        raster_protocol::LayerTransport::AndroidHardwareBuffer;
    const bool byteAddressedLayer = shmLayer || dmaBufLayer;
    const bool hasTransportStorage = nativeBufferLayer
        ? static_cast<bool>(layer.nativeBuffer)
        : layer.fd >= 0;
    if (!hasTransportStorage || ready.layerId == 0 ||
        ready.width == 0 || ready.height == 0 ||
        !SurfaceRegistry::matchesConfiguredBufferExtent(
            entry, ready.width, ready.height) ||
        ready.backingWidth < ready.width ||
        ready.backingHeight < ready.height ||
        ready.backingWidth >
            std::numeric_limits<uint32_t>::max() / sizeof(uint32_t) ||
        // A GPU-only AHardwareBuffer is opaque storage. Some valid Android
        // implementations report no CPU row stride, and texture import does
        // not consume one. SHM and DMA-BUF remain byte-addressed and retain
        // the strict row-size validation.
        (byteAddressedLayer &&
            ready.stride < ready.backingWidth * sizeof(uint32_t)) ||
        ready.damageWidth == 0 || ready.damageHeight == 0 ||
        ready.damageX > ready.width || ready.damageY > ready.height ||
        ready.damageWidth > ready.width - ready.damageX ||
        ready.damageHeight > ready.height - ready.damageY ||
        (!shmLayer && !dmaBufLayer && !nativeBufferLayer) ||
        (shmLayer && ready.byteSize !=
            static_cast<uint64_t>(ready.stride) * ready.backingHeight) ||
        (shmLayer && ready.bufferId != 0) ||
        ((dmaBufLayer || nativeBufferLayer) &&
            (ready.format == 0 || ready.bufferId == 0)) ||
        (nativeBufferLayer && ready.byteSize != 0)) {
        (void)LayerFeedbackHandler::discard(
            entry.clientFd, ready,
            protocol::LCLFrameDiscardReason::InvalidFrame);
        closeLayer(raster_protocol::LayerReleaseReason::RejectedFrame);
        return false;
    }
    if (entry.ignoreBufferCommits || entry.pendingDestroy ||
        !SurfaceRegistry::acceptsBufferCommit(entry, ready.configureSerial) ||
        (ready.geometryGeneration != 0 &&
         ready.geometryGeneration != entry.configuredGeometryGeneration)) {
        (void)LayerFeedbackHandler::discard(
            entry.clientFd, ready,
            entry.ignoreBufferCommits || entry.pendingDestroy
                ? protocol::LCLFrameDiscardReason::SurfaceClosed
                : protocol::LCLFrameDiscardReason::Superseded);
        closeLayer(entry.ignoreBufferCommits || entry.pendingDestroy
            ? raster_protocol::LayerReleaseReason::SurfaceRevoked
            : raster_protocol::LayerReleaseReason::RejectedFrame);
        return false;
    }
    void* pixels = nullptr;
    uint32_t texture = 0;
    if (shmLayer) {
        struct stat info{};
        if (fstat(layer.fd, &info) != 0 ||
            static_cast<uint64_t>(info.st_size) != ready.byteSize) {
            (void)LayerFeedbackHandler::discard(
                entry.clientFd, ready,
                protocol::LCLFrameDiscardReason::InvalidFrame);
            closeLayer(raster_protocol::LayerReleaseReason::RejectedFrame);
            return false;
        }
        pixels = mmap(nullptr, ready.byteSize, PROT_READ, MAP_SHARED,
                      layer.fd, 0);
        if (pixels == MAP_FAILED) {
            (void)LayerFeedbackHandler::discard(
                entry.clientFd, ready,
                protocol::LCLFrameDiscardReason::InvalidFrame);
            closeLayer(raster_protocol::LayerReleaseReason::RejectedFrame);
            return false;
        }
    } else if (dmaBufLayer) {
        platform::DmaBufDescriptor descriptor{};
        descriptor.fd = layer.fd;
        descriptor.width = ready.backingWidth;
        descriptor.height = ready.backingHeight;
        descriptor.stride = ready.stride;
        descriptor.format = ready.format;
        descriptor.modifier = ready.modifier;
        texture = m_renderer.getRasterRenderer()->importDmaBuf(
            ready.bufferId, descriptor);
        if (texture == 0) {
            std::cerr << "[LCL Raster] Desktop DMA-BUF import rejected for "
                         "surface "
                      << ready.grant.surfaceId
                      << "; requesting desktop SHM fallback\n";
            (void)LayerFeedbackHandler::discard(
                entry.clientFd, ready,
                protocol::LCLFrameDiscardReason::InvalidFrame);
            closeLayer(raster_protocol::LayerReleaseReason::RejectedTransport);
            return false;
        }
        close(layer.fd);
        layer.fd = -1;
    } else {
        if (layer.fd >= 0) {
            const auto wait = m_renderer.waitNativeFence(layer.fd);
            if (wait == platform::NativeFenceWaitResult::Unsupported) {
                close(layer.fd);
                layer.fd = -1;
                std::cerr << "[LCL Raster] Native acquire fence import "
                             "unsupported for surface "
                          << ready.grant.surfaceId << "\n";
                (void)LayerFeedbackHandler::discard(
                    entry.clientFd, ready,
                    protocol::LCLFrameDiscardReason::InvalidFrame);
                closeLayer(
                    raster_protocol::LayerReleaseReason::RejectedTransport);
                return false;
            }
            // Enqueued and ConsumedFailure transfer descriptor ownership to
            // the graphics backend.
            layer.fd = -1;
            if (wait == platform::NativeFenceWaitResult::ConsumedFailure) {
                (void)LayerFeedbackHandler::discard(
                    entry.clientFd, ready,
                    protocol::LCLFrameDiscardReason::InvalidFrame);
                closeLayer(
                    raster_protocol::LayerReleaseReason::RejectedTransport);
                return false;
            }
        }
        texture = m_renderer.getRasterRenderer()->importNativeBuffer(
            ready.bufferId, std::move(layer.nativeBuffer));
        if (texture == 0) {
            std::cerr << "[LCL Raster] Native-buffer import rejected for "
                         "surface "
                      << ready.grant.surfaceId << "\n";
            (void)LayerFeedbackHandler::discard(
                entry.clientFd, ready,
                protocol::LCLFrameDiscardReason::InvalidFrame);
            closeLayer(
                raster_protocol::LayerReleaseReason::RejectedTransport);
            return false;
        }
    }

    if (entry.rasterLayerId != 0) {
        entry.pendingRasterLayerReleases.push_back(
            {entry.rasterLayerId, entry.rasterLayerTexture});
    }
    if (entry.pixels && entry.shmSize > 0) munmap(entry.pixels, entry.shmSize);
    if (entry.shmFd >= 0) close(entry.shmFd);
    entry.pixels = pixels;
    entry.shmFd = shmLayer ? layer.fd : -1;
    if (shmLayer) layer.fd = -1;
    entry.shmSize = shmLayer ? ready.byteSize : 0;
    entry.width = ready.width;
    entry.height = ready.height;
    entry.backingWidth = ready.backingWidth;
    entry.backingHeight = ready.backingHeight;
    entry.stride = ready.stride;
    entry.shmDamageX = ready.damageX;
    entry.shmDamageY = ready.damageY;
    entry.shmDamageWidth = ready.damageWidth;
    entry.shmDamageHeight = ready.damageHeight;
    entry.shmContentSerial = m_nextShmContentSerial++;
    entry.rasterLayerId = ready.layerId;
    entry.rasterLayerTexture = texture;
    entry.frameSerial = ready.frameSerial;
    entry.layerGeometryGeneration = ready.geometryGeneration;
    entry.clientFrameStartNs = ready.clientFrameStartNs;
    entry.clientSubmitNs = ready.clientSubmitNs;
    entry.rasterStartNs = ready.rasterStartNs;
    entry.rasterReadyNs = ready.rasterReadyNs;
    entry.acceptedConfigureSerial = ready.configureSerial;

    if (entry.isPopup() || entry.isAttached()) {
        entry.hasCommittedBuffer = true;
    } else if (!entry.hasCommittedBuffer &&
               !mapSurface(surface->first, entry, ready.grant.ownerPid)) {
        // mapSurface() is the last step of the first-frame transaction. Do not
        // leave a released rasterd slot mapped in an otherwise-unmapped entry
        // if policy rejects that map.
        SurfaceRegistry::releaseBuffer(entry);
        if (entry.rasterLayerTexture != 0) {
            m_renderer.getRasterRenderer()->releaseDmaBufTexture(
                entry.rasterLayerTexture);
        }
        entry.rasterLayerId = 0;
        entry.rasterLayerTexture = 0;
        entry.frameSerial = 0;
        entry.layerGeometryGeneration = 0;
        entry.clientFrameStartNs = 0;
        entry.clientSubmitNs = 0;
        entry.rasterStartNs = 0;
        entry.rasterReadyNs = 0;
        entry.acceptedConfigureSerial = 0;
        entry.width = entry.height = 0;
        entry.backingWidth = entry.backingHeight = 0;
        entry.stride = 0;
        entry.hasCommittedBuffer = false;
        for (const auto& stale : entry.pendingRasterLayerReleases) {
            const int releaseFenceFd = stale.texture != 0
                ? m_renderer.createNativeFence() : -1;
            if (stale.texture != 0) {
                m_renderer.getRasterRenderer()->releaseDmaBufTexture(
                    stale.texture);
            }
            m_rasterService.releaseLayer(
                stale.layerId,
                raster_protocol::LayerReleaseReason::Presented,
                releaseFenceFd);
        }
        entry.pendingRasterLayerReleases.clear();
        closeLayer(raster_protocol::LayerReleaseReason::RejectedFrame);
        return false;
    }
    SurfaceRegistry::queuePresentation(entry, ready.configureSerial);
    // Atomic WindowGroup geometry is committed only after every participant is
    // ready. Non-atomic initial/focus-only frames may update immediately.
    if (entry.atomicConfigureGeneration == 0) {
        (void)commitClientSurfaceGeometry(entry);
    }
    return true;
}

bool ProtocolDispatcher::process(IPCManager& ipcManager) {
    bool changed = false;
    auto toRenderDecorationMode = [](protocol::LCLDecorationMode mode) {
        switch (mode) {
            case protocol::LCLDecorationMode::CSD: return render::DecorationMode::CSD;
            case protocol::LCLDecorationMode::None: return render::DecorationMode::None;
            case protocol::LCLDecorationMode::SSD:
            default: return render::DecorationMode::SSD;
        }
    };
    auto beginClosingTransition = [&](SurfaceEntry& entry) {
        if (!SurfaceRegistry::beginClosingTransition(entry)) return false;
        m_windowManager.transferFocusFromWindow(entry.windowId);
        std::cout << "[LCL Compositor] Closing transition started for Window ID: "
                  << entry.windowId << "\n";
        changed = true;
        return true;
    };

    auto destroyPopupChildren = [&](uint64_t parentSurfaceKey) {
        for (const auto childKey : m_surfaces.popupChildren(parentSurfaceKey)) {
            auto child = m_surfaces.find(childKey);
            if (child == m_surfaces.end()) continue;
            if (child->second.clientFd >= 0) {
                lcl::protocol::LCLHeader header{};
                header.opcode = lcl::protocol::LCLOpcode::SurfaceDestroy;
                header.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceDestroy);
                lcl::protocol::LCLMsgSurfaceDestroy destroy{};
                destroy.surfaceId = static_cast<uint32_t>(childKey & 0xFFFFFFFFu);
                lcl::protocol::sendMsgWithFd(child->second.clientFd, header, &destroy);
            }
            child->second.ignoreBufferCommits = true;
            child->second.pendingDestroy = true;
            m_surfaces.releaseKeyboardFocus(childKey);
            changed = true;
        }
    };

    auto requestSurfaceClose = [&](uint64_t surfaceKey, uint32_t surfaceId) {
        auto it = m_surfaces.find(surfaceKey);
        if (it == m_surfaces.end()) return;

        if (!it->second.isAttached()) destroyPopupChildren(surfaceKey);

        // Ask the client to stop its loop while compositor animates its frozen
        // last frame.  All close entry points use this same transition path.
        if (it->second.clientFd >= 0) {
            lcl::protocol::LCLHeader destroyHeader{};
            destroyHeader.opcode = lcl::protocol::LCLOpcode::SurfaceDestroy;
            destroyHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceDestroy);
            lcl::protocol::LCLMsgSurfaceDestroy destroyMsg{};
            destroyMsg.surfaceId = surfaceId;
            lcl::protocol::sendMsgWithFd(it->second.clientFd, destroyHeader, &destroyMsg);
        }

        m_surfaces.releaseKeyboardFocus(surfaceKey);
        if (it->second.isPopup() || it->second.isAttached()) {
            it->second.ignoreBufferCommits = true;
            it->second.pendingDestroy = true;
            changed = true;
        } else if (!beginClosingTransition(it->second)) {
            revokeRasterSurface(it->second);
            publishLaunchIconVisibility(it->second, true);
            if (it->second.windowId > 0) {
                m_windowManager.removeWindow(it->second.windowId);
            }
            m_scenes.removeSurface(surfaceKey);
            m_surfaces.erase(it);
            changed = true;
        } else {
            m_scenes.markClosing(surfaceKey);
        }
    };

    auto applyManagedWindowAction = [&] (
            SurfaceRegistry::Key surfaceKey,
            protocol::LCLWindowAction action,
            float localX, float localY) {
        const auto surfaceIt = m_surfaces.find(surfaceKey);
        if (surfaceIt == m_surfaces.end() || surfaceIt->second.isPopup() ||
            surfaceIt->second.isAttached()) return;

        const uint32_t windowId = surfaceIt->second.windowId;
        const auto currentWindow = std::find_if(
            m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
            [windowId](const auto& window) { return window.id == windowId; });
        const bool hasCurrentWindow =
            currentWindow != m_windowManager.getWindows().end();
        switch (action) {
            case protocol::LCLWindowAction::BeginDrag:
                if (const auto interaction = m_windowManager.beginWindowDrag(
                        windowId, localX, localY)) {
                    SurfaceRegistry::interruptGeometryTransaction(
                        surfaceIt->second, interaction.generation);
                    changed = true;
                }
                break;
            case protocol::LCLWindowAction::Minimize:
                if (windowId != 0 &&
                    surfaceIt->second.transitionPhase ==
                        SurfaceEntry::TransitionPhase::None) {
                    surfaceIt->second.transitionPhase =
                        SurfaceEntry::TransitionPhase::Minimizing;
                    surfaceIt->second.transitionElapsedSec = 0.0f;
                    surfaceIt->second.transitionDurationSec = 0.18f;
                    surfaceIt->second.transitionOpacity = 1.0f;
                    surfaceIt->second.transitionScale = 1.0f;
                    m_windowManager.transferFocusFromWindow(windowId);
                    changed = true;
                }
                break;
            case protocol::LCLWindowAction::Maximize:
                if (m_windowManager.maximizeWindow(windowId)) {
                    changed = true;
                }
                break;
            case protocol::LCLWindowAction::Restore:
                if (hasCurrentWindow && currentWindow->isMinimized) {
                    if (m_windowManager.restoreWindow(windowId)) {
                        surfaceIt->second.transitionPhase =
                            SurfaceEntry::TransitionPhase::Restoring;
                        surfaceIt->second.transitionElapsedSec = 0.0f;
                        surfaceIt->second.transitionDurationSec = 0.22f;
                        surfaceIt->second.transitionOpacity = 0.0f;
                        surfaceIt->second.transitionScale = 0.92f;
                        changed = true;
                    }
                } else if (m_windowManager.restoreWindow(windowId)) {
                    changed = true;
                }
                break;
            case protocol::LCLWindowAction::ToggleMaximize:
                if (m_windowManager.toggleMaximizeWindow(windowId)) {
                    changed = true;
                }
                break;
            case protocol::LCLWindowAction::Close:
                requestSurfaceClose(
                    surfaceKey, static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu));
                break;
            default:
                break;
        }
    };

    for (const auto& msg : ipcManager.pollMessages()) {
        if (msg.disconnected) {
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);
            m_shellSubscriptions.erase(msg.clientFd);
            std::vector<uint64_t> surfacesToRemove;
            std::vector<uint64_t> ownedParentSurfaces;
            for (const auto& [surfKey, entry] : m_surfaces) {
                if (entry.isLaunchPlaceholder &&
                    entry.launchOwnerFd == msg.clientFd) {
                    if (entry.windowId != 0) {
                        m_windowManager.removeWindow(entry.windowId);
                    }
                    surfacesToRemove.push_back(surfKey);
                    continue;
                }
                if (SurfaceRegistry::isOwnedByClientConnection(entry, msg.clientFd) &&
                    !entry.isPopup() && !entry.isAttached()) {
                    ownedParentSurfaces.push_back(surfKey);
                }
            }
            for (const auto parentKey : ownedParentSurfaces) {
                destroyPopupChildren(parentKey);
            }
            for (auto& [surfKey, entry] : m_surfaces) {
                // One process can own wallpaper, menu and Dock over separate
                // sockets. Closing one WindowApp must not tear down every
                // surface sharing that PID; process exit closes each socket
                // and therefore still cleans all of them deterministically.
                if (SurfaceRegistry::isOwnedByClientConnection(entry, msg.clientFd)) {
                    m_surfaces.releaseKeyboardFocus(surfKey);
                    revokeRasterSurface(entry);
                    entry.clientFd = -1;
                    if (entry.isPopup() || entry.isAttached()) {
                        entry.ignoreBufferCommits = true;
                        entry.pendingDestroy = true;
                    } else if (!beginClosingTransition(entry)) {
                        publishLaunchIconVisibility(entry, true);
                        if (entry.windowId > 0) m_windowManager.removeWindow(entry.windowId);
                        surfacesToRemove.push_back(surfKey);
                    } else {
                        m_scenes.markClosing(surfKey);
                    }
                }
            }
            for (uint64_t key : surfacesToRemove) {
                if (auto entry = m_surfaces.find(key); entry != m_surfaces.end()) {
                    revokeRasterSurface(entry->second);
                }
                m_scenes.removeSurface(key);
                m_surfaces.erase(key);
            }
            changed = true;
            continue;
        }

        if (msg.header.opcode == lcl::protocol::LCLOpcode::AckResponse) continue;
        RequestAck requestAck(msg.clientFd, msg.header.requestId);

        // --- SURFACE_CREATE: register a window on the compositor canvas ---
        const bool isSurfaceCreate = msg.header.opcode == lcl::protocol::LCLOpcode::SurfaceCreate;
        const bool isPopupSurfaceCreate =
            msg.header.opcode == lcl::protocol::LCLOpcode::PopupSurfaceCreate;
        const bool isAttachedSurfaceCreate =
            msg.header.opcode == lcl::protocol::LCLOpcode::AttachedSurfaceCreate;

        if (msg.header.opcode == lcl::protocol::LCLOpcode::SetSystemSurfaceKind) {
            if (msg.payload.size() != sizeof(lcl::protocol::LCLMsgSetSystemSurfaceKind)) {
                requestAck.error(3, "invalid system-surface declaration");
                continue;
            }
            const auto* declaration = reinterpret_cast<const lcl::protocol::LCLMsgSetSystemSurfaceKind*>(msg.payload.data());
            if (!SystemSurfacePolicyRegistry::isValidKind(declaration->kind) ||
                !SystemSurfacePolicyRegistry::isTrustedShellPeer(msg.pid)) {
                requestAck.error(5, "system-surface declaration denied");
                continue;
            }
            m_pendingSystemSurfaceKinds[msg.clientFd] = declaration->kind;
            continue;
        }

        if (msg.header.opcode == lcl::protocol::LCLOpcode::SubscribeShellState) {
            if (msg.payload.size() != sizeof(lcl::protocol::LCLMsgSubscribeShellState)) {
                requestAck.error(3, "invalid shell-state subscription");
                continue;
            }
            if (!SystemSurfacePolicyRegistry::isTrustedShellPeer(msg.pid)) {
                requestAck.error(4, "shell-state subscription requires a trusted shell peer");
                continue;
            }
            const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgSubscribeShellState*>(msg.payload.data());
            m_shellSubscriptions[msg.clientFd] = ShellSubscription{request->lastKnownRevision, false};
            changed = true;
            continue;
        }

        const auto trustedHomeSurface = [&](uint32_t homeSurfaceId) {
            const auto key = SurfaceRegistry::makeKey(
                msg.clientFd, msg.pid, homeSurfaceId);
            const auto home = m_surfaces.find(key);
            return home != m_surfaces.end() &&
                home->second.clientFd == msg.clientFd &&
                home->second.systemSurfaceKind ==
                    protocol::LCLSystemSurfaceKind::HomeScreen;
        };
        const auto findLaunch = [&](uint64_t launchToken) {
            return std::find_if(
                m_surfaces.begin(), m_surfaces.end(),
                [launchToken](const auto& item) {
                    return item.second.launchToken == launchToken;
                });
        };

        if (msg.header.opcode ==
            lcl::protocol::LCLOpcode::BeginLaunchPlaceholder) {
            const auto* request = reinterpret_cast<const
                lcl::protocol::LCLMsgBeginLaunchPlaceholder*>(
                    msg.payload.data());
            if (!trustedHomeSurface(request->homeSurfaceId)) {
                requestAck.error(5, "launch placeholder requires HomeScreen");
                continue;
            }
            constexpr uint64_t kLaunchPlaceholderKeyBit = uint64_t{1} << 63;
            if ((request->launchToken & kLaunchPlaceholderKeyBit) != 0) {
                requestAck.error(3, "launch token is out of range");
                continue;
            }
            const size_t iconPixelCount =
                static_cast<size_t>(request->iconWidth) * request->iconHeight;
            const auto* iconBytes = msg.payload.data() + sizeof(*request);

            // A process may still be starting without a surface when its icon
            // is tapped again. Keep one visual placeholder per mobile app and
            // let the app-instance identity join whichever first surface wins.
            for (auto old = m_surfaces.begin(); old != m_surfaces.end();) {
                if (old->second.isLaunchPlaceholder &&
                    old->second.launchOwnerFd == msg.clientFd &&
                    old->second.appId == request->appId) {
                    if (old->second.windowId != 0) {
                        m_windowManager.removeWindow(old->second.windowId);
                    }
                    old = m_surfaces.erase(old);
                } else {
                    ++old;
                }
            }
            const float width = m_windowManager.getScreenWidth();
            const float height = m_windowManager.getScreenHeight();
            const auto configureLaunchWindow = [&](SurfaceEntry& entry) {
                if (entry.windowId == 0) {
                    entry.windowId = m_windowManager.createWindow(
                        request->appId, 0.0f, 0.0f, width, height, true);
                }
                m_windowManager.setDecorationMode(
                    entry.windowId, render::DecorationMode::None);
                m_windowManager.setEdgeToEdge(entry.windowId, true);
                m_windowManager.setInsetBorderEnabled(entry.windowId, false);
                m_windowManager.setWindowCornerStyle(
                    entry.windowId, 0.0f, 2.0f);
                entry.initialX = 0.0f;
                entry.initialY = 0.0f;
                entry.initialWidth = width;
                entry.initialHeight = height;
                entry.configuredX = 0.0f;
                entry.configuredY = 0.0f;
                entry.configuredWidth = width;
                entry.configuredHeight = height;
                entry.decorationMode = protocol::LCLDecorationMode::None;
                entry.edgeToEdge = true;
                entry.insetBorderEnabled = false;
                entry.cornerRadius = 0.0f;
                entry.launchToken = request->launchToken;
                entry.launchOwnerFd = msg.clientFd;
                entry.launchHomeTransitionProgress = 0.0f;
                // The session service has not told us whether this is a new
                // instance yet. Keep the icon morph alive, but do not expose
                // the white first-launch placeholder until resolution. A
                // reused surface already owns the content it must present.
                entry.launchPlaceholderActive = false;
                entry.launchContentOpacity = 0.0f;
                entry.launchContentFadeElapsedSec = 0.0f;
                entry.launchContentFadeActive = false;
                entry.hasLaunchOrigin = true;
                entry.launchOriginX = request->originX;
                entry.launchOriginY = request->originY;
                entry.launchOriginWidth = request->originWidth;
                entry.launchOriginHeight = request->originHeight;
                entry.launchOriginCornerRadius =
                    request->originCornerRadius;
                entry.launchIconWidth = request->iconWidth;
                entry.launchIconHeight = request->iconHeight;
                entry.launchIconPixels.resize(iconPixelCount);
                std::memcpy(entry.launchIconPixels.data(), iconBytes,
                            iconPixelCount * sizeof(uint32_t));
                entry.launchIconRevealPending = false;
                entry.launchIconHandoffActive = false;
                entry.launchIconHandoffDeadline = {};
                entry.transitionPhase = SurfaceEntry::TransitionPhase::Entering;
                entry.transitionElapsedSec = 0.0f;
                entry.transitionDurationSec =
                    lcl::motion::tokens::windowOpen().tweenParams.durationSec;
                entry.transitionOpacity = 0.0f;
                entry.transitionScale = 1.0f;
                entry.launchMorphActive = false;
            };

            // SurfaceCreate travels on the child socket and can win the poll
            // race against this HomeScreen socket. Adopt that registered or
            // already-mapped surface instead of creating a second window.
            const auto earlySurface = findLaunch(request->launchToken);
            if (earlySurface != m_surfaces.end()) {
                if (earlySurface->second.isLaunchPlaceholder ||
                    earlySurface->second.launchOwnerFd >= 0) {
                    requestAck.error(8, "launch token is already active");
                    continue;
                }
                configureLaunchWindow(earlySurface->second);
                changed = true;
                continue;
            }

            const SurfaceRegistry::Key placeholderKey =
                kLaunchPlaceholderKeyBit | request->launchToken;
            if (m_surfaces.contains(placeholderKey)) {
                requestAck.error(8, "launch placeholder key collision");
                continue;
            }

            SurfaceEntry placeholder{};
            placeholder.title = request->appId;
            placeholder.appId = request->appId;
            placeholder.isLaunchPlaceholder = true;
            configureLaunchWindow(placeholder);
            m_surfaces[placeholderKey] = std::move(placeholder);
            changed = true;
            continue;
        }

        if (msg.header.opcode ==
            lcl::protocol::LCLOpcode::ResolveLaunchPlaceholder) {
            const auto* request = reinterpret_cast<const
                lcl::protocol::LCLMsgResolveLaunchPlaceholder*>(
                    msg.payload.data());
            if (!trustedHomeSurface(request->homeSurfaceId)) {
                requestAck.error(5, "launch resolution requires HomeScreen");
                continue;
            }
            auto launch = findLaunch(request->launchToken);
            if (launch == m_surfaces.end()) {
                requestAck.error(6, "launch placeholder is unavailable");
                continue;
            }
            if (launch->second.launchOwnerFd != msg.clientFd) {
                requestAck.error(5, "launch placeholder is not owned by HomeScreen");
                continue;
            }
            launch->second.appInstanceId = request->appInstanceId;
            if (request->reused != 0 &&
                launch->second.isLaunchPlaceholder) {
                auto existing = std::find_if(
                    m_surfaces.begin(), m_surfaces.end(),
                    [&](const auto& item) {
                        return item.first != launch->first &&
                            !item.second.isPopup() &&
                            !item.second.isLaunchPlaceholder &&
                            item.second.appInstanceId == request->appInstanceId &&
                            item.second.windowId != 0;
                    });
                if (existing != m_surfaces.end()) {
                    const float originX = launch->second.launchOriginX;
                    const float originY = launch->second.launchOriginY;
                    const float originWidth = launch->second.launchOriginWidth;
                    const float originHeight = launch->second.launchOriginHeight;
                    const float originRadius =
                        launch->second.launchOriginCornerRadius;
                    const uint32_t iconWidth = launch->second.launchIconWidth;
                    const uint32_t iconHeight = launch->second.launchIconHeight;
                    auto iconPixels = std::move(
                        launch->second.launchIconPixels);
                    const uint32_t placeholderWindowId = launch->second.windowId;
                    m_windowManager.removeWindow(placeholderWindowId);
                    m_surfaces.erase(launch);

                    auto& target = existing->second;
                    if (!m_windowManager.restoreWindow(target.windowId, false)) {
                        m_windowManager.focusWindow(target.windowId);
                    }
                    target.launchToken = request->launchToken;
                    target.launchOwnerFd = msg.clientFd;
                    target.launchHomeTransitionProgress = 0.0f;
                    target.hasLaunchOrigin = true;
                    target.launchOriginX = originX;
                    target.launchOriginY = originY;
                    target.launchOriginWidth = originWidth;
                    target.launchOriginHeight = originHeight;
                    target.launchOriginCornerRadius = originRadius;
                    target.launchIconWidth = iconWidth;
                    target.launchIconHeight = iconHeight;
                    target.launchIconPixels = std::move(iconPixels);
                    target.launchIconRevealPending = false;
                    target.launchIconHandoffActive = false;
                    target.launchIconHandoffDeadline = {};
                    target.pendingMinimize = false;
                    target.transitionPhase =
                        SurfaceEntry::TransitionPhase::Restoring;
                    target.transitionElapsedSec = 0.0f;
                    target.transitionDurationSec =
                        lcl::motion::tokens::restore().tweenParams.durationSec;
                    target.transitionOpacity = 0.0f;
                    target.transitionScale = 1.0f;
                    target.launchMorphActive = false;
                    // Restoring a retained single-instance surface must morph
                    // its existing buffer directly. The white placeholder is
                    // reserved for an instance that has no first frame yet.
                    target.launchPlaceholderActive = false;
                    target.launchContentOpacity = 1.0f;
                    target.launchContentFadeElapsedSec = 0.0f;
                    target.launchContentFadeActive = false;
                } else {
                    // The process can be single-instance while still waiting
                    // to create its first surface. In that case there is no
                    // retained content to restore, so keep the first-launch
                    // placeholder until that surface is adopted by instance id.
                    launch->second.launchPlaceholderActive = true;
                    launch->second.launchContentOpacity = 1.0f;
                    launch->second.launchContentFadeElapsedSec = 0.0f;
                    launch->second.launchContentFadeActive = false;
                }
            } else if (request->reused == 0) {
                // Only a newly launched instance gets the white placeholder.
                // If its first frame won the SurfaceCreate race, fade that
                // frame over the placeholder from this point onward.
                launch->second.launchPlaceholderActive = true;
                launch->second.launchContentOpacity =
                    launch->second.hasRenderableBuffer() ? 0.0f : 1.0f;
                launch->second.launchContentFadeElapsedSec = 0.0f;
                launch->second.launchContentFadeActive =
                    launch->second.hasRenderableBuffer();
            }
            changed = true;
            continue;
        }

        if (msg.header.opcode ==
            lcl::protocol::LCLOpcode::LaunchIconVisibilityAck) {
            const auto* ack = reinterpret_cast<const
                lcl::protocol::LCLMsgLaunchIconVisibilityAck*>(
                    msg.payload.data());
            const auto launch = findLaunch(ack->launchToken);
            if (launch == m_surfaces.end() ||
                launch->second.launchOwnerFd != msg.clientFd ||
                launch->second.appId != ack->appId ||
                !launch->second.launchIconHandoffActive) {
                requestAck.error(6, "launch icon handoff is unavailable");
                continue;
            }
            launch->second.launchIconRevealPending = false;
            launch->second.launchIconHandoffActive = false;
            launch->second.launchIconHandoffDeadline = {};
            launch->second.launchMorphActive = false;
            changed = true;
            continue;
        }

        if (msg.header.opcode ==
            lcl::protocol::LCLOpcode::CancelLaunchPlaceholder) {
            const auto* request = reinterpret_cast<const
                lcl::protocol::LCLMsgCancelLaunchPlaceholder*>(
                    msg.payload.data());
            if (!trustedHomeSurface(request->homeSurfaceId)) {
                requestAck.error(5, "launch cancellation requires HomeScreen");
                continue;
            }
            const auto launch = findLaunch(request->launchToken);
            if (launch != m_surfaces.end() &&
                launch->second.isLaunchPlaceholder &&
                launch->second.launchOwnerFd == msg.clientFd) {
                m_windowManager.removeWindow(launch->second.windowId);
                m_surfaces.erase(launch);
                changed = true;
            }
            continue;
        }

        if (isAttachedSurfaceCreate) {
            if (!SystemSurfacePolicyRegistry::isTrustedShellPeer(msg.pid)) {
                std::cerr << "[LCL Compositor ERROR] Attached surface denied for PID "
                          << msg.pid << ": untrusted WM peer\n";
                requestAck.error(5, "attached surfaces require trusted WM capability");
                continue;
            }
            if (msg.payload.size() !=
                sizeof(lcl::protocol::LCLMsgAttachedSurfaceCreate)) {
                requestAck.error(3, "invalid attached surface request");
                continue;
            }
            const auto* attached = reinterpret_cast<const
                lcl::protocol::LCLMsgAttachedSurfaceCreate*>(msg.payload.data());
            const bool validGeometry = std::isfinite(attached->x) &&
                std::isfinite(attached->y) &&
                std::isfinite(attached->width) &&
                std::isfinite(attached->height) &&
                attached->width > 0.0f && attached->height > 0.0f;
            const auto parent = std::find_if(
                m_windowManager.getWindows().begin(),
                m_windowManager.getWindows().end(),
                [attached](const auto& window) {
                    return window.id == attached->targetWindowId;
                });
            if (!validGeometry ||
                parent == m_windowManager.getWindows().end()) {
                std::cerr << "[LCL Compositor ERROR] Attached surface target "
                          << attached->targetWindowId << " is unavailable\n";
                requestAck.error(7, "attached surface target is unavailable");
                continue;
            }

            const auto surfaceKey = SurfaceRegistry::makeKey(
                msg.clientFd, msg.pid, attached->surfaceId);
            if (m_surfaces.contains(surfaceKey)) {
                requestAck.error(8, "attached surface ID is already registered");
                continue;
            }

            SurfaceEntry entry{};
            entry.attachedWindowId = attached->targetWindowId;
            entry.attachedRole = attached->role;
            entry.attachedX = attached->x;
            entry.attachedY = attached->y;
            entry.attachedWidth = attached->width;
            entry.attachedHeight = attached->height;
            entry.attachedFollowParentWidth = attached->followParentWidth != 0;
            entry.attachedFollowParentHeight = attached->followParentHeight != 0;
            entry.attachedAcceptsInput = attached->acceptsInput != 0;
            entry.attachmentOrder = m_surfaces.allocateAttachmentOrder();
            entry.windowId = attached->targetWindowId;
            entry.initialWidth = attached->width;
            entry.initialHeight = attached->height;
            entry.clientFd = msg.clientFd;
            entry.bufferScale = m_renderer.getRasterRenderer()->getDeviceScale();
            entry.decorationMode = protocol::LCLDecorationMode::None;
            entry.insetBorderEnabled = false;
            entry.suppressInitialTransition = true;
            entry.unfocusable = true;
            m_surfaces[surfaceKey] = std::move(entry);

            auto& configured = m_surfaces[surfaceKey];
            grantRasterSurface(configured, attached->surfaceId, msg.pid, true);
            const float configuredWidth = configured.attachedFollowParentWidth
                ? parent->width : configured.attachedWidth;
            const float configuredHeight = configured.attachedFollowParentHeight
                ? parent->height : configured.attachedHeight;
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);
            protocol::LCLMsgConfigureBounds configure{};
            configure.surfaceId = attached->surfaceId;
            configure.configureSerial = configured.nextConfigureSerial++;
            configure.geometryGeneration = parent->geometryGeneration;
            configure.x = configured.attachedX;
            configure.y = configured.attachedY;
            configure.width = configuredWidth;
            configure.height = configuredHeight;
            configure.backingWidth = configuredWidth;
            configure.backingHeight = configuredHeight;
            configure.bufferScale = configured.bufferScale;
            configure.resizeReason = protocol::LCLConfigureResizeReason::Initial;
            configured.pendingConfigureSerial = configure.configureSerial;
            configured.configuredX = configured.attachedX;
            configured.configuredY = configured.attachedY;
            configured.configuredWidth = configuredWidth;
            configured.configuredHeight = configuredHeight;
            configured.configuredGeometryGeneration =
                parent->geometryGeneration;
            protocol::sendMsgWithFd(msg.clientFd, header, &configure);
            changed = true;

        } else if (isPopupSurfaceCreate) {
            const auto* popup = reinterpret_cast<const lcl::protocol::LCLMsgPopupSurfaceCreate*>(
                msg.payload.data());
            const auto owner = static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd);
            const uint64_t surfaceKey = (owner << 32) | popup->surfaceId;
            const uint64_t parentKey = (owner << 32) | popup->parentSurfaceId;
            const auto parent = m_surfaces.find(parentKey);
            if (parent == m_surfaces.end() || parent->second.isPopup() ||
                parent->second.pendingDestroy || parent->second.ignoreBufferCommits ||
                parent->second.transitionPhase == SurfaceEntry::TransitionPhase::Closing) {
                requestAck.error(7, "popup parent surface is unavailable");
                continue;
            }
            const uint32_t parentWindowId = parent->second.windowId;
            if (m_surfaces.contains(surfaceKey)) {
                requestAck.error(8, "popup surface ID is already registered");
                continue;
            }

            SurfaceEntry entry{};
            entry.parentSurfaceKey = parentKey;
            entry.popupRole = popup->role;
            entry.popupX = popup->x;
            entry.popupY = popup->y;
            entry.popupOrder = m_surfaces.allocatePopupOrder();
            entry.initialWidth = popup->width;
            entry.initialHeight = popup->height;
            entry.clientFd = msg.clientFd;
            entry.bufferScale = m_renderer.getRasterRenderer()->getDeviceScale();
            entry.decorationMode = protocol::LCLDecorationMode::None;
            entry.insetBorderEnabled = true;
            entry.suppressInitialTransition = true;
            m_surfaces[surfaceKey] = std::move(entry);
            if (parentWindowId != 0) {
                m_windowManager.focusWindow(parentWindowId);
            }
            m_surfaces.focusKeyboardSurface(surfaceKey);

            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);
            protocol::LCLMsgConfigureBounds configure{};
            auto& configured = m_surfaces[surfaceKey];
            grantRasterSurface(configured, popup->surfaceId, msg.pid, false);
            configure.surfaceId = popup->surfaceId;
            configure.configureSerial = configured.nextConfigureSerial++;
            configure.geometryGeneration = parentWindowId != 0
                ? std::find_if(
                      m_windowManager.getWindows().begin(),
                      m_windowManager.getWindows().end(),
                      [parentWindowId](const auto& window) {
                          return window.id == parentWindowId;
                      })->geometryGeneration
                : 1;
            configure.x = popup->x;
            configure.y = popup->y;
            configure.width = popup->width;
            configure.height = popup->height;
            configure.backingWidth = popup->width;
            configure.backingHeight = popup->height;
            configure.bufferScale = configured.bufferScale;
            configure.resizeReason = protocol::LCLConfigureResizeReason::Initial;
            configure.isFocused = 1;
            configured.pendingConfigureSerial = configure.configureSerial;
            configured.configuredX = configured.popupX;
            configured.configuredY = configured.popupY;
            configured.configuredWidth = configured.initialWidth;
            configured.configuredHeight = configured.initialHeight;
            configured.configuredFocused = configure.isFocused;
            configured.configuredGeometryGeneration =
                configure.geometryGeneration;
            protocol::sendMsgWithFd(msg.clientFd, header, &configure);
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);
            changed = true;

        } else if (isSurfaceCreate) {
            uint32_t surfId = 1;
            std::string title = "LCL Application";
            float winX = 80.0f;
            float winY = 60.0f;
            float winW = 540.0f;
            float winH = 360.0f;
            const float bufferScale = m_renderer.getRasterRenderer()->getDeviceScale();
            uint64_t launchToken = 0;
            uint64_t appInstanceId = 0;
            std::string appId;
            bool hasLaunchOrigin = false;
            float launchOriginX = 0.0f;
            float launchOriginY = 0.0f;
            float launchOriginWidth = 0.0f;
            float launchOriginHeight = 0.0f;
            float launchOriginCornerRadius = 0.0f;
            float resizeBaseWidth = 0.0f;
            float resizeBaseHeight = 0.0f;
            float resizeWidthIncrement = 0.0f;
            float resizeHeightIncrement = 0.0f;

            if (msg.payload.size() == sizeof(lcl::protocol::LCLMsgSurfaceCreate)) {
                auto* sm = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data());
                surfId = sm->surfaceId;
                if (sm->title[0]) title = sm->title;
                winX = sm->x;
                winY = sm->y;
                winW = (sm->width > 0.0f) ? sm->width
                                          : m_windowManager.getScreenWidth();
                winH = (sm->height > 0.0f) ? sm->height
                                           : m_windowManager.getScreenHeight();
                launchToken = sm->launchToken;
                appInstanceId = sm->appInstanceId;
                appId = sm->appId;
                hasLaunchOrigin = sm->hasLaunchOrigin != 0;
                launchOriginX = sm->launchOriginX;
                launchOriginY = sm->launchOriginY;
                launchOriginWidth = sm->launchOriginWidth;
                launchOriginHeight = sm->launchOriginHeight;
                launchOriginCornerRadius = sm->launchOriginCornerRadius;
                resizeBaseWidth = sm->resizeBaseWidth;
                resizeBaseHeight = sm->resizeBaseHeight;
                resizeWidthIncrement = sm->resizeWidthIncrement;
                resizeHeightIncrement = sm->resizeHeightIncrement;
            }

            auto kindIt = m_pendingSystemSurfaceKinds.find(msg.clientFd);
            const protocol::LCLSystemSurfaceKind requestedSystemKind =
                kindIt != m_pendingSystemSurfaceKinds.end()
                    ? kindIt->second
                    : protocol::LCLSystemSurfaceKind::None;
            const auto systemPolicy = SystemSurfacePolicyRegistry::policyFor(requestedSystemKind);
            SystemSurfacePolicyRegistry::applyInitialPlacement(
                systemPolicy, m_windowManager.getScreenWidth(),
                m_windowManager.getScreenHeight(),
                winX, winY, winW, winH);
            protocol::LCLDecorationMode initialDecorationMode =
                protocol::LCLDecorationMode::SSD;
            bool initialInsetBorderEnabled = true;
            if (!systemPolicy.isSystemSurface) {
                m_windowingPolicy.configureNormalSurface(
                    m_windowManager.getScreenWidth(),
                    m_windowManager.getScreenHeight(),
                    winX, winY, winW, winH,
                    initialDecorationMode, initialInsetBorderEnabled);
            }

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                SurfaceEntry entry{};
                bool adoptedPlaceholder = false;
                auto placeholder = launchToken != 0
                    ? findLaunch(launchToken)
                    : m_surfaces.end();
                if (placeholder == m_surfaces.end() && appInstanceId != 0) {
                    placeholder = std::find_if(
                        m_surfaces.begin(), m_surfaces.end(),
                        [appInstanceId](const auto& item) {
                            return item.second.isLaunchPlaceholder &&
                                item.second.appInstanceId == appInstanceId;
                        });
                }
                if (placeholder != m_surfaces.end() &&
                    placeholder->second.isLaunchPlaceholder) {
                    entry = std::move(placeholder->second);
                    m_surfaces.erase(placeholder);
                    entry.isLaunchPlaceholder = false;
                    adoptedPlaceholder = true;
                }
                entry.title = title;
                entry.initialX = winX;
                entry.initialY = winY;
                entry.initialWidth = winW;
                entry.initialHeight = winH;
                entry.resizeBaseWidth = resizeBaseWidth;
                entry.resizeBaseHeight = resizeBaseHeight;
                entry.resizeWidthIncrement = resizeWidthIncrement;
                entry.resizeHeightIncrement = resizeHeightIncrement;
                entry.systemSurfaceKind = requestedSystemKind;
                entry.suppressInitialTransition = systemPolicy.suppressInitialTransition;
                entry.decorationMode = systemPolicy.isSystemSurface
                    ? protocol::LCLDecorationMode::None
                    : initialDecorationMode;
                entry.insetBorderEnabled = systemPolicy.isSystemSurface
                    ? systemPolicy.insetBorderEnabled
                    : initialInsetBorderEnabled;
                if (systemPolicy.isSystemSurface) {
                    entry.layer = systemPolicy.layer;
                    entry.unfocusable = systemPolicy.unfocusable;
                }
                entry.clientFd = msg.clientFd;
                entry.bufferScale = bufferScale;
                entry.appId = std::move(appId);
                entry.appInstanceId = appInstanceId;
                if (!adoptedPlaceholder) entry.launchToken = launchToken;
                if (hasLaunchOrigin && !adoptedPlaceholder) {
                    entry.hasLaunchOrigin = true;
                    entry.launchOriginX = launchOriginX;
                    entry.launchOriginY = launchOriginY;
                    entry.launchOriginWidth = launchOriginWidth;
                    entry.launchOriginHeight = launchOriginHeight;
                    entry.launchOriginCornerRadius =
                        launchOriginCornerRadius;
                }
                m_surfaces[surfaceKey] = std::move(entry);
                std::cout << "[LCL Compositor] Registered unmapped Surface " << surfId
                          << " from client PID " << msg.pid << " (" << winW << "x" << winH << ")\n";
            } else {
                auto& entry = m_surfaces[surfaceKey];
                entry.clientFd = msg.clientFd;
                entry.resizeBaseWidth = resizeBaseWidth;
                entry.resizeBaseHeight = resizeBaseHeight;
                entry.resizeWidthIncrement = resizeWidthIncrement;
                entry.resizeHeightIncrement = resizeHeightIncrement;
                if (entry.windowId != 0) {
                    (void)m_windowManager.setResizeConstraints(
                        entry.windowId,
                        {resizeBaseWidth, resizeBaseHeight,
                         resizeWidthIncrement, resizeHeightIncrement});
                }
            }
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);

            // Immediately send ConfigureBounds back to client so client knows assigned window dimensions
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

            protocol::LCLMsgConfigureBounds cfgMsg{};
            cfgMsg.surfaceId = surfId;
            auto& configuredEntry = m_surfaces[surfaceKey];
            grantRasterSurface(
                configuredEntry, surfId, msg.pid,
                configuredEntry.systemSurfaceKind !=
                    protocol::LCLSystemSurfaceKind::None);
            cfgMsg.configureSerial = configuredEntry.nextConfigureSerial++;
            cfgMsg.geometryGeneration = 1;
            configuredEntry.pendingConfigureSerial = cfgMsg.configureSerial;
            cfgMsg.x = winX;
            cfgMsg.y = winY;
            cfgMsg.width = winW;
            cfgMsg.height = winH;
            cfgMsg.backingWidth = cfgMsg.width;
            cfgMsg.backingHeight = cfgMsg.height;
            cfgMsg.bufferScale = bufferScale;
            cfgMsg.resizeReason = protocol::LCLConfigureResizeReason::Initial;
            cfgMsg.isFocused = 1;

            configuredEntry.configuredX = winX;
            configuredEntry.configuredY = winY;
            configuredEntry.configuredWidth = winW;
            configuredEntry.configuredHeight = winH;
            configuredEntry.configuredFocused = cfgMsg.isFocused;
            configuredEntry.configuredGeometryGeneration =
                cfgMsg.geometryGeneration;

            protocol::sendMsgWithFd(msg.clientFd, header, &cfgMsg);

        // --- UPLOAD_IMAGE_RESOURCE: copy immutable pixels into compositor ownership ---
        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetDecorationMode) {
            uint32_t surfId = 1;
            lcl::protocol::LCLDecorationMode mode = lcl::protocol::LCLDecorationMode::SSD;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetDecorationMode)) {
                auto* decMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetDecorationMode*>(msg.payload.data());
                surfId = decMsg->surfaceId;
                mode = decMsg->mode;
            }
            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            auto it = m_surfaces.find(surfaceKey);
            if (it != m_surfaces.end()) {
                mode = it->second.systemSurfaceKind ==
                           protocol::LCLSystemSurfaceKind::None
                    ? m_windowingPolicy.resolveDecorationMode(mode)
                    : protocol::LCLDecorationMode::None;
                it->second.decorationMode = mode;
                const auto wmMode = toRenderDecorationMode(mode);
                if (it->second.windowId != 0) {
                    m_windowManager.setDecorationMode(it->second.windowId, wmMode);
                }
                std::cout << "[LCL Compositor] Set decoration mode for Surface " << surfId
                          << " to " << (wmMode == render::DecorationMode::None ? "None (Frameless)" : (wmMode == render::DecorationMode::CSD ? "CSD" : "SSD")) << "\n";
                changed = true;
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetEdgeToEdge) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetEdgeToEdge)) {
                const auto* edgeToEdgeMsg =
                    reinterpret_cast<const lcl::protocol::LCLMsgSetEdgeToEdge*>(
                        msg.payload.data());
                const uint64_t surfaceKey =
                    (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) |
                    edgeToEdgeMsg->surfaceId;
                const auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    it->second.edgeToEdge = edgeToEdgeMsg->enabled != 0;
                    if (it->second.windowId != 0) {
                        m_windowManager.setEdgeToEdge(
                            it->second.windowId, edgeToEdgeMsg->enabled != 0);
                    }
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::RequestWindowAction) {
            if (msg.payload.size() < sizeof(lcl::protocol::LCLMsgRequestWindowAction)) {
                continue;
            }

            const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgRequestWindowAction*>(msg.payload.data());
            const bool desktopOnlyAction =
                request->action == lcl::protocol::LCLWindowAction::BeginDrag ||
                request->action == lcl::protocol::LCLWindowAction::Maximize ||
                request->action == lcl::protocol::LCLWindowAction::Restore ||
                request->action == lcl::protocol::LCLWindowAction::ToggleMaximize;
            if (desktopOnlyAction &&
                !m_windowingPolicy.usesDesktopWindowManagement()) {
                requestAck.error(5, "window action unavailable in mobile mode");
                continue;
            }
            const uint64_t surfaceKey =
                (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | request->surfaceId;
            const auto surfaceIt = m_surfaces.find(surfaceKey);
            if (surfaceIt == m_surfaces.end()) {
                continue;
            }
            applyManagedWindowAction(surfaceKey, request->action,
                                     request->localX, request->localY);

        } else if (msg.header.opcode ==
                   lcl::protocol::LCLOpcode::RequestManagedWindowAction) {
            if (!SystemSurfacePolicyRegistry::isTrustedShellPeer(msg.pid)) {
                requestAck.error(5, "managed window action requires trusted WM capability");
                continue;
            }
            if (msg.payload.size() != sizeof(
                    lcl::protocol::LCLMsgRequestManagedWindowAction)) {
                requestAck.error(3, "invalid managed window action");
                continue;
            }
            const auto* request = reinterpret_cast<const
                lcl::protocol::LCLMsgRequestManagedWindowAction*>(
                    msg.payload.data());
            const bool desktopOnlyAction =
                request->action == lcl::protocol::LCLWindowAction::BeginDrag ||
                request->action == lcl::protocol::LCLWindowAction::Maximize ||
                request->action == lcl::protocol::LCLWindowAction::Restore ||
                request->action ==
                    lcl::protocol::LCLWindowAction::ToggleMaximize;
            if (desktopOnlyAction &&
                !m_windowingPolicy.usesDesktopWindowManagement()) {
                requestAck.error(5, "window action unavailable in mobile mode");
                continue;
            }
            const auto target = std::find_if(
                m_surfaces.begin(), m_surfaces.end(),
                [request](const auto& item) {
                    return !item.second.isPopup() && !item.second.isAttached() &&
                        item.second.windowId == request->targetWindowId;
                });
            if (target == m_surfaces.end()) {
                requestAck.error(7, "managed window target is unavailable");
                continue;
            }
            applyManagedWindowAction(target->first, request->action,
                                     request->localX, request->localY);

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::BeginWindowMove) {
            if (!m_windowingPolicy.usesDesktopWindowManagement()) {
                requestAck.error(5, "window move unavailable in mobile mode");
                continue;
            }
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgBeginWindowMove)) {
                auto* moveMsg = reinterpret_cast<const lcl::protocol::LCLMsgBeginWindowMove*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | moveMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end() && it->second.windowId > 0) {
                    if (const auto interaction = m_windowManager.beginWindowDrag(
                        it->second.windowId, moveMsg->localX, moveMsg->localY)) {
                        SurfaceRegistry::interruptGeometryTransaction(
                            it->second, interaction.generation);
                        changed = true;
                    }
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::RequestSurfaceClose) {
            uint32_t surfId = 1;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgRequestSurfaceClose)) {
                auto* closeMsg = reinterpret_cast<const lcl::protocol::LCLMsgRequestSurfaceClose*>(msg.payload.data());
                surfId = closeMsg->surfaceId;
            }

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            requestSurfaceClose(surfaceKey, surfId);

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowLayer) {
            uint32_t surfId = 1;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowLayer)) {
                auto* layerMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowLayer*>(msg.payload.data());
                surfId = layerMsg->surfaceId;
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    if (it->second.systemSurfaceKind != protocol::LCLSystemSurfaceKind::None) {
                        requestAck.error(6, "system surface policy is compositor-owned");
                        continue;
                    }
                    it->second.layer = layerMsg->layer;
                    it->second.unfocusable = layerMsg->unfocusable != 0;

                    // Default policy: wallpaper and unfocusable top overlays (menu bar) do not get forced inset borders.
                    const bool disableInsetBorder =
                        (layerMsg->layer == lcl::protocol::LCLWindowLayer::Bottom) ||
                        (layerMsg->layer == lcl::protocol::LCLWindowLayer::TopMost && layerMsg->unfocusable != 0);
                    it->second.insetBorderEnabled =
                        m_windowingPolicy.resolveInsetBorderEnabled(
                            !disableInsetBorder);
                    if (it->second.windowId != 0) {
                        m_windowManager.setWindowLayer(it->second.windowId, it->second.layer, it->second.unfocusable);
                        m_windowManager.setInsetBorderEnabled(it->second.windowId, it->second.insetBorderEnabled);
                    }

                    std::cout << "[LCL Compositor] Set window layer for Surface " << surfId
                              << " to " << static_cast<uint32_t>(layerMsg->layer)
                              << " (unfocusable=" << static_cast<int>(layerMsg->unfocusable) << ")\n";
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetInsetBorder) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetInsetBorder)) {
                auto* borderMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetInsetBorder*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | borderMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    it->second.insetBorderEnabled =
                        m_windowingPolicy.resolveInsetBorderEnabled(
                            borderMsg->enabled != 0);
                    if (it->second.windowId != 0) {
                        m_windowManager.setInsetBorderEnabled(it->second.windowId, it->second.insetBorderEnabled);
                    }
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowCornerRadius) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowCornerRadius)) {
                auto* radiusMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerRadius*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | radiusMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    it->second.cornerRadius =
                        m_windowingPolicy.resolveWindowCornerRadius(
                            radiusMsg->radius);
                    if (it->second.windowId != 0) {
                        m_windowManager.setWindowCornerStyle(it->second.windowId,
                                                              it->second.cornerRadius,
                                                              it->second.cornerRoundness);
                    }
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowCornerStyle) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowCornerStyle)) {
                auto* styleMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerStyle*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | styleMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    it->second.cornerRadius =
                        m_windowingPolicy.resolveWindowCornerRadius(
                            styleMsg->radius);
                    it->second.cornerRoundness = std::clamp(styleMsg->roundness, 2.0f, 8.0f);
                    if (it->second.windowId != 0) {
                        m_windowManager.setWindowCornerStyle(it->second.windowId,
                                                              it->second.cornerRadius,
                                                              it->second.cornerRoundness);
                    }
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetReservedZone) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetReservedZone)) {
                auto* resMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetReservedZone*>(msg.payload.data());
                const uint64_t surfaceKey =
                    (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | resMsg->surfaceId;
                const auto surface = m_surfaces.find(surfaceKey);
                if (surface != m_surfaces.end() &&
                    surface->second.systemSurfaceKind != protocol::LCLSystemSurfaceKind::None) {
                    requestAck.error(6, "system reserved zone is compositor-owned");
                    continue;
                }
                m_windowManager.setReservedZone(resMsg->top, resMsg->bottom, resMsg->left, resMsg->right);
                changed = true;
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetEffectGraph) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader)) {
                auto* graphHeader = reinterpret_cast<const lcl::protocol::LCLMsgSetEffectGraphHeader*>(msg.payload.data());
                size_t expectedSize = sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader)
                    + static_cast<size_t>(graphHeader->regionCount) * sizeof(lcl::protocol::EffectRegion)
                    + static_cast<size_t>(graphHeader->filterCount) * sizeof(lcl::protocol::FilterOp);

                if (msg.payload.size() >= expectedSize) {
                    const uint8_t* base = msg.payload.data() + sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader);
                    const auto* regions = reinterpret_cast<const lcl::protocol::EffectRegion*>(base);
                    const auto* filters = reinterpret_cast<const lcl::protocol::FilterOp*>(
                        base + static_cast<size_t>(graphHeader->regionCount) * sizeof(lcl::protocol::EffectRegion));

                    uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | graphHeader->surfaceId;
                    auto it = m_surfaces.find(surfaceKey);
                    if (it != m_surfaces.end()) {
                        std::vector<SurfaceEffectRegion> parsed;
                        parsed.reserve(graphHeader->regionCount);

                        bool valid = true;
                        for (uint32_t i = 0; i < graphHeader->regionCount; ++i) {
                            const auto& region = regions[i];
                            size_t offset = static_cast<size_t>(region.filterOffset);
                            size_t count = static_cast<size_t>(region.filterCount);
                            if (offset + count > static_cast<size_t>(graphHeader->filterCount) ||
                                !std::isfinite(region.cornerRadius) || region.cornerRadius < 0.0f ||
                                !std::isfinite(region.cornerRoundness) ||
                                region.cornerRoundness < 2.0f || region.cornerRoundness > 8.0f ||
                                (region.boundsPolicy != lcl::protocol::EffectBoundsPolicy::Local &&
                                 region.boundsPolicy != lcl::protocol::EffectBoundsPolicy::OuterSurface) ||
                                !std::isfinite(region.opacity) ||
                                region.opacity < 0.0f || region.opacity > 1.0f) {
                                valid = false;
                                break;
                            }

                            SurfaceEffectRegion dstRegion;
                            dstRegion.region = region;
                            dstRegion.filters.assign(filters + offset, filters + offset + count);
                            dstRegion.followSurfaceBounds = effectMatchesLogicalSurfaceBounds(
                                region, it->second.configuredWidth,
                                it->second.configuredHeight);
                            parsed.push_back(std::move(dstRegion));
                        }

                        if (valid) {
                            it->second.effectRegions = std::move(parsed);
                            ++it->second.effectRevision;
                            if (it->second.effectRevision == 0) {
                                ++it->second.effectRevision;
                            }
                            changed = true;
                        }
                    }
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::ClearEffectGraph) {
            uint32_t surfId = 1;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgClearEffectGraph)) {
                auto* clearMsg = reinterpret_cast<const lcl::protocol::LCLMsgClearEffectGraph*>(msg.payload.data());
                surfId = clearMsg->surfaceId;
            }

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            auto it = m_surfaces.find(surfaceKey);
            if (it != m_surfaces.end()) {
                it->second.effectRegions.clear();
                ++it->second.effectRevision;
                if (it->second.effectRevision == 0) {
                    ++it->second.effectRevision;
                }
                changed = true;
            }

        } else {
            requestAck.error(2, "opcode not accepted from client");
        }
    }

    if (changed) recomputeSystemReservedZone();
    return changed;
}

void ProtocolDispatcher::recomputeSystemReservedZone() {
    const auto zone = SystemSurfacePolicyRegistry::computeReservedZone(m_surfaces);
    m_windowManager.setReservedZone(zone.top, zone.bottom, 0.0f, 0.0f);
}

void ProtocolDispatcher::publishShellStateToSubscribers() {
    const auto snapshot = m_shellState.snapshot(m_scenes, m_focus);
    for (auto& [fd, subscription] : m_shellSubscriptions) {
        if (!subscription.hasDeliveredState || subscription.revision > snapshot.revision) {
            protocol::LCLMsgShellStateSnapshot headerPayload{};
            headerPayload.revision = snapshot.revision;
            headerPayload.sceneCount = static_cast<uint32_t>(snapshot.scenes.size());
            headerPayload.seatId = snapshot.focus.seatId;
            headerPayload.displayId = snapshot.focus.displayId;
            headerPayload.workspaceId = snapshot.focus.workspaceId;
            headerPayload.activeSceneId = snapshot.focus.activeSceneId;
            std::vector<uint8_t> payload(sizeof(headerPayload) +
                snapshot.scenes.size() * sizeof(protocol::LCLMsgShellScene));
            std::memcpy(payload.data(), &headerPayload, sizeof(headerPayload));
            size_t offset = sizeof(headerPayload);
            for (const auto& scene : snapshot.scenes) {
                const auto wire = toWireScene(scene);
                std::memcpy(payload.data() + offset, &wire, sizeof(wire));
                offset += sizeof(wire);
            }
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ShellStateSnapshot;
            header.payloadSize = static_cast<uint32_t>(payload.size());
            if (protocol::sendMsgWithFd(fd, header, payload.data())) {
                subscription.revision = snapshot.revision;
                subscription.hasDeliveredState = true;
            }
            continue;
        }

        const auto batch = m_shellState.deltasSince(subscription.revision);
        if (batch.requiresSnapshot) {
            subscription.hasDeliveredState = false;
            continue;
        }
        for (const auto& delta : batch.deltas) {
            const auto payload = toWireDelta(delta);
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ShellStateDelta;
            header.payloadSize = sizeof(payload);
            if (!protocol::sendMsgWithFd(fd, header, &payload)) {
                break;
            }
            subscription.revision = delta.revision;
        }
    }
}

} // namespace lcl::core
