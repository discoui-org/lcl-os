#include "core/compositor/protocol_dispatcher.hpp"
#include "core/compositor/effect_region_geometry.hpp"
#include "lcl-motion/motion.hpp"
#include "lcl-theme/theme.hpp"
#include "platform/common/native_buffer.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include "platform/android/ahardware_native_buffer.hpp"
#endif

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

ProtocolDispatcher::~ProtocolDispatcher() {
    for (const auto& [clientFd, channelFd] : m_nativeBufferChannels) {
        (void)clientFd;
        if (channelFd >= 0) close(channelFd);
    }
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
    auto mapSurface = [&](SurfaceRegistry::Key surfaceKey, SurfaceEntry& entry, pid_t clientPid) {
        if (entry.isPopup() || !entry.hasRenderableBuffer() ||
            entry.width == 0 || entry.height == 0) {
            return false;
        }

        if (entry.windowId != 0) {
            if (!entry.hasCommittedBuffer) {
                entry.hasCommittedBuffer = true;
                if (entry.launchPlaceholderActive) {
                    entry.launchContentOpacity = 0.0f;
                    entry.launchContentFadeElapsedSec = 0.0f;
                    entry.launchContentFadeActive = true;
                }
                if (entry.systemSurfaceKind ==
                    protocol::LCLSystemSurfaceKind::None) {
                    m_scenes.mapClientSurface(
                        surfaceKey, clientPid, entry.windowId,
                        entry.appId, entry.title, entry.appInstanceId);
                }
            }
            return true;
        }

        const auto decorationMode = toRenderDecorationMode(entry.decorationMode);
        const int titleOffset = decorationMode == render::DecorationMode::SSD
            ? 32
            : 0;
        entry.windowId = m_windowManager.createWindow(
            entry.title, entry.initialX, entry.initialY,
            entry.initialWidth,
            entry.initialHeight + static_cast<float>(titleOffset),
            ::lcl::theme::defaultTheme().colors.windowTitleFocused.toARGB(),
            !entry.unfocusable);
        m_windowManager.setDecorationMode(entry.windowId, decorationMode);
        m_windowManager.setEdgeToEdge(entry.windowId, entry.edgeToEdge);
        m_windowManager.setWindowLayer(entry.windowId, entry.layer, entry.unfocusable);
        m_windowManager.setInsetBorderEnabled(entry.windowId, entry.insetBorderEnabled);
        m_windowManager.setResizePresentationMode(entry.windowId, entry.resizePresentation);
        if (entry.cornerRadius >= 0.0f) {
            m_windowManager.setWindowCornerStyle(entry.windowId, entry.cornerRadius,
                                                  entry.cornerRoundness);
        }

        if (entry.suppressInitialTransition) {
            entry.transitionPhase = SurfaceEntry::TransitionPhase::None;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
        } else {
            entry.transitionPhase = SurfaceEntry::TransitionPhase::Entering;
            entry.transitionElapsedSec = 0.0f;
            entry.transitionDurationSec = lcl::motion::tokens::windowOpen().tweenParams.durationSec;
            entry.transitionOpacity = 0.0f;
            entry.transitionScale = 0.96f;
        }
        entry.hasCommittedBuffer = true;
        if (entry.systemSurfaceKind == protocol::LCLSystemSurfaceKind::None) {
            m_scenes.mapClientSurface(surfaceKey, clientPid, entry.windowId,
                                      entry.appId, entry.title,
                                      entry.appInstanceId);
        }
        std::cout << "[LCL Compositor] Mapped Window (ID: " << entry.windowId
                  << ") after first client buffer commit\n";
        return true;
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

        destroyPopupChildren(surfaceKey);

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
        if (it->second.isPopup()) {
            it->second.ignoreBufferCommits = true;
            it->second.pendingDestroy = true;
            changed = true;
        } else if (!beginClosingTransition(it->second)) {
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

    for (const auto& msg : ipcManager.pollMessages()) {
        if (msg.disconnected) {
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);
            m_shellSubscriptions.erase(msg.clientFd);
            if (const auto channel = m_nativeBufferChannels.find(msg.clientFd);
                channel != m_nativeBufferChannels.end()) {
                if (channel->second >= 0) close(channel->second);
                m_nativeBufferChannels.erase(channel);
            }
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
                    !entry.isPopup()) {
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
                    entry.clientFd = -1;
                    if (entry.isPopup()) {
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
        const bool isAttachBuffer = msg.header.opcode == lcl::protocol::LCLOpcode::AttachBuffer;
        const bool isAttachDmaBuf = msg.header.opcode == lcl::protocol::LCLOpcode::AttachDmaBuf;
        const bool isAttachNativeBuffer =
            msg.header.opcode == lcl::protocol::LCLOpcode::AttachNativeBuffer;

        if (msg.header.opcode == lcl::protocol::LCLOpcode::QueryCapabilities) {
            if (msg.payload.size() !=
                sizeof(lcl::protocol::LCLMsgQueryCapabilities)) {
                requestAck.error(3, "invalid capability query");
                continue;
            }
            const auto* query = reinterpret_cast<const
                lcl::protocol::LCLMsgQueryCapabilities*>(msg.payload.data());
            lcl::protocol::LCLMsgCapabilities capabilities{};
            int clientChannelFd = -1;
#if defined(__ANDROID__)
            if ((query->requested & lcl::protocol::LCL_CAPABILITY_AHB_V1) != 0) {
                int channels[2]{-1, -1};
                if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                               0, channels) == 0) {
                    if (const auto old = m_nativeBufferChannels.find(msg.clientFd);
                        old != m_nativeBufferChannels.end()) {
                        if (old->second >= 0) close(old->second);
                        m_nativeBufferChannels.erase(old);
                    }
                    m_nativeBufferChannels[msg.clientFd] = channels[0];
                    clientChannelFd = channels[1];
                    capabilities.supported |=
                        lcl::protocol::LCL_CAPABILITY_AHB_V1;
                }
            }
#else
            (void)query;
#endif
            lcl::protocol::LCLHeader response{};
            response.opcode = lcl::protocol::LCLOpcode::Capabilities;
            response.payloadSize = sizeof(capabilities);
            if (!lcl::protocol::sendMsgWithFd(
                    msg.clientFd, response, &capabilities, clientChannelFd)) {
                requestAck.error(4, "capability response unavailable");
            }
            if (clientChannelFd >= 0) close(clientChannelFd);
            continue;
        }

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
                        request->appId, 0.0f, 0.0f, width, height,
                        ::lcl::theme::defaultTheme().colors
                            .windowTitleFocused.toARGB(),
                        true);
                }
                m_windowManager.setDecorationMode(
                    entry.windowId, render::DecorationMode::None);
                m_windowManager.setEdgeToEdge(entry.windowId, true);
                m_windowManager.setInsetBorderEnabled(entry.windowId, false);
                m_windowManager.setWindowCornerStyle(
                    entry.windowId, 0.0f, 2.0f);
                m_windowManager.setResizePresentationMode(
                    entry.windowId,
                    protocol::LCLResizePresentationMode::CompositorMorph);
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
                entry.launchPlaceholderActive = true;
                entry.launchContentOpacity = 0.0f;
                entry.launchContentFadeElapsedSec = 0.0f;
                entry.launchContentFadeActive = entry.hasRenderableBuffer();
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
            placeholder.forceOpaque = true;
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
                    target.launchPlaceholderActive = true;
                    target.launchContentOpacity = 0.0f;
                    target.launchContentFadeElapsedSec = 0.0f;
                    target.launchContentFadeActive = true;
                }
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

        if (isPopupSurfaceCreate) {
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
            configure.surfaceId = popup->surfaceId;
            configure.configureSerial = configured.nextConfigureSerial++;
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
            protocol::LCLResizePresentationMode resizePresentation =
                protocol::LCLResizePresentationMode::CompositorMorph;
            uint64_t launchToken = 0;
            uint64_t appInstanceId = 0;
            std::string appId;
            bool hasLaunchOrigin = false;
            float launchOriginX = 0.0f;
            float launchOriginY = 0.0f;
            float launchOriginWidth = 0.0f;
            float launchOriginHeight = 0.0f;
            float launchOriginCornerRadius = 0.0f;

            if (msg.payload.size() == sizeof(lcl::protocol::LCLMsgSurfaceCreate)) {
                auto* sm = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data());
                surfId = sm->surfaceId;
                if (sm->title[0]) title = sm->title;
                resizePresentation = sm->resizePresentation;
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
                entry.resizePresentation = resizePresentation;
                entry.systemSurfaceKind = requestedSystemKind;
                entry.suppressInitialTransition = systemPolicy.suppressInitialTransition;
                entry.decorationMode = systemPolicy.isSystemSurface
                    ? protocol::LCLDecorationMode::None
                    : initialDecorationMode;
                entry.insetBorderEnabled = systemPolicy.isSystemSurface
                    ? systemPolicy.insetBorderEnabled
                    : initialInsetBorderEnabled;
                entry.forceOpaque = !systemPolicy.isSystemSurface &&
                    m_windowingPolicy.forcesOpaqueNormalSurfaces();
                if (entry.forceOpaque) entry.cornerRadius = 0.0f;
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
                m_surfaces[surfaceKey].clientFd = msg.clientFd;
            }
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);

            // Immediately send ConfigureBounds back to client so client knows assigned window dimensions
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

            protocol::LCLMsgConfigureBounds cfgMsg{};
            cfgMsg.surfaceId = surfId;
            auto& configuredEntry = m_surfaces[surfaceKey];
            cfgMsg.configureSerial = configuredEntry.nextConfigureSerial++;
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

            protocol::sendMsgWithFd(msg.clientFd, header, &cfgMsg);

        // --- ATTACH_DMA_BUF: import one client GBM allocation as a GPU texture ---
        } else if (isAttachDmaBuf) {
            if (msg.payload.size() != sizeof(lcl::protocol::LCLMsgAttachDmaBuf) || msg.passedFd < 0) {
                if (msg.passedFd >= 0) close(msg.passedFd);
                requestAck.error(3, "invalid DMA-BUF attach");
                continue;
            }
            const auto* bufferMessage = reinterpret_cast<const lcl::protocol::LCLMsgAttachDmaBuf*>(msg.payload.data());
            const uint64_t surfaceKey =
                (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) |
                bufferMessage->surfaceId;
            auto surfaceIt = m_surfaces.find(surfaceKey);
            if (surfaceIt == m_surfaces.end()) {
                close(msg.passedFd);
                continue;
            }
            auto& entry = surfaceIt->second;
            const auto releaseRejected = [&] {
                lcl::protocol::LCLHeader releaseHeader{};
                releaseHeader.opcode = lcl::protocol::LCLOpcode::ReleaseDmaBuf;
                releaseHeader.payloadSize = sizeof(lcl::protocol::LCLMsgReleaseDmaBuf);
                lcl::protocol::LCLMsgReleaseDmaBuf release{};
                release.surfaceId = bufferMessage->surfaceId;
                release.bufferId = bufferMessage->bufferId;
                lcl::protocol::sendMsgWithFd(msg.clientFd, releaseHeader, &release);
            };
            if (entry.ignoreBufferCommits || entry.transitionPhase == SurfaceEntry::TransitionPhase::Closing ||
                !SurfaceRegistry::acceptsBufferCommit(entry, bufferMessage->configureSerial)) {
                close(msg.passedFd);
                // Stale live frames are normal under latest-serial coalescing.
                // Return ownership without diagnostics or importing the fd.
                releaseRejected();
                continue;
            }

            lcl::platform::DmaBufDescriptor descriptor{};
            descriptor.fd = msg.passedFd;
            descriptor.width = bufferMessage->backingWidth;
            descriptor.height = bufferMessage->backingHeight;
            descriptor.stride = bufferMessage->stride;
            descriptor.format = bufferMessage->format;
            descriptor.modifier = bufferMessage->modifier;
            const uint32_t texture = m_renderer.getRasterRenderer()->importDmaBuf(descriptor);
            close(msg.passedFd);
            if (texture == 0) {
                // Keep the client pool live when a compositor lacks DMA-BUF
                // import support; its next frame can use the SHM fallback.
                lcl::protocol::LCLHeader releaseHeader{};
                releaseHeader.opcode = lcl::protocol::LCLOpcode::ReleaseDmaBuf;
                releaseHeader.payloadSize = sizeof(lcl::protocol::LCLMsgReleaseDmaBuf);
                lcl::protocol::LCLMsgReleaseDmaBuf release{};
                release.surfaceId = bufferMessage->surfaceId;
                release.bufferId = bufferMessage->bufferId;
                lcl::protocol::sendMsgWithFd(msg.clientFd, releaseHeader, &release);
                requestAck.error(4, "DMA-BUF import unavailable");
                continue;
            }

            if (entry.resizeTransitionPhase == SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer &&
                entry.hasRenderableBuffer()) {
                SurfaceRegistry::releasePreviousBuffer(entry);
                entry.previousPixels = entry.pixels;
                entry.previousShmSize = entry.shmSize;
                entry.previousShmFd = entry.shmFd;
                entry.previousWidth = entry.width;
                entry.previousHeight = entry.height;
                entry.previousBackingWidth = entry.backingWidth;
                entry.previousBackingHeight = entry.backingHeight;
                entry.previousStride = entry.stride;
                entry.previousShmContentSerial = entry.shmContentSerial;
                entry.previousDmaBufId = entry.dmaBufId;
                entry.previousDmaBufTexture = entry.dmaBufTexture;
                entry.pixels = nullptr;
                entry.shmSize = 0;
                entry.shmFd = -1;
                entry.shmContentSerial = 0;
                entry.dmaBufId = 0;
                entry.dmaBufTexture = 0;
            } else {
                if (entry.dmaBufTexture != 0 || entry.dmaBufId != 0) {
                    entry.pendingDmaBufReleases.push_back({entry.dmaBufId, entry.dmaBufTexture});
                }
                if (entry.pixels && entry.shmSize > 0) munmap(entry.pixels, entry.shmSize);
                if (entry.shmFd >= 0) close(entry.shmFd);
                entry.pixels = nullptr;
                entry.shmSize = 0;
                entry.shmFd = -1;
                entry.shmContentSerial = 0;
                entry.dmaBufId = 0;
                entry.dmaBufTexture = 0;
            }
            entry.clientFd = msg.clientFd;
            entry.dmaBufId = bufferMessage->bufferId;
            entry.dmaBufTexture = texture;
            if (!entry.dmaBufTransportActive) {
                entry.dmaBufTransportActive = true;
                std::cerr << "[LCL DMA-BUF] Surface " << bufferMessage->surfaceId
                          << " zero-copy import active\n";
            }
            entry.width = bufferMessage->width;
            entry.height = bufferMessage->height;
            entry.backingWidth = bufferMessage->backingWidth;
            entry.backingHeight = bufferMessage->backingHeight;
            entry.stride = bufferMessage->stride;
            entry.acceptedConfigureSerial = bufferMessage->configureSerial;
            if (entry.resizeTransitionPhase == SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer) {
                entry.resizeTransitionPhase = SurfaceEntry::ResizeTransitionPhase::Crossfading;
                entry.resizeCrossfadeElapsedSec = 0.0f;
                entry.resizeCrossfadeProgress = 0.0f;
            }
            if (entry.isPopup()) {
                entry.hasCommittedBuffer = true;
            } else if (!entry.hasCommittedBuffer &&
                       !mapSurface(surfaceKey, entry, msg.pid)) {
                continue;
            }

            if (entry.isPopup()) {
                SurfaceRegistry::queuePresentation(entry, bufferMessage->configureSerial);
                changed = true;
                continue;
            }

            int titleOffset = 0;
            for (const auto& win : m_windowManager.getWindows()) {
                if (win.id == entry.windowId && win.decorationMode == render::DecorationMode::SSD) {
                    titleOffset = 32;
                    break;
                }
            }
            const float requestedContentW = entry.configuredWidth;
            const float requestedContentH = entry.configuredHeight;
            const float committedContentW = SurfaceRegistry::committedLogicalExtent(
                entry.width, requestedContentW, entry.bufferScale);
            const float committedContentH = SurfaceRegistry::committedLogicalExtent(
                entry.height, requestedContentH, entry.bufferScale);
            const float frameW = committedContentW;
            const float frameH = committedContentH + static_cast<float>(titleOffset);
            bool preserveNewerTarget = false;
            const auto configuredWindow = std::find_if(
                m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
                [&entry](const auto& window) { return window.id == entry.windowId; });
            if (configuredWindow != m_windowManager.getWindows().end()) {
                const float configuredFrameHeight =
                    requestedContentH + static_cast<float>(titleOffset);
                preserveNewerTarget = configuredWindow->pendingWidth != requestedContentW ||
                    configuredWindow->pendingHeight != configuredFrameHeight;
            }
            entry.configuredWidth = committedContentW;
            entry.configuredHeight = committedContentH;
            m_windowManager.commitSurfaceGeometry(entry.windowId, frameW, frameH,
                                                  preserveNewerTarget, entry.configuredX,
                                                  entry.configuredY,
                                                  entry.configuredGeometryGeneration);
            SurfaceRegistry::queuePresentation(
                entry, bufferMessage->configureSerial);
            changed = true;

        // --- ATTACH_NATIVE_BUFFER: import an opaque AHB as a GPU texture ---
        } else if (isAttachNativeBuffer) {
#if defined(__ANDROID__)
            if (msg.payload.size() !=
                sizeof(lcl::protocol::LCLMsgAttachNativeBuffer)) {
                if (msg.passedFd >= 0) close(msg.passedFd);
                requestAck.error(3, "invalid native-buffer attach");
                continue;
            }
            const auto* bufferMessage = reinterpret_cast<const
                lcl::protocol::LCLMsgAttachNativeBuffer*>(msg.payload.data());
            const auto channel = m_nativeBufferChannels.find(msg.clientFd);
            if (channel == m_nativeBufferChannels.end() || channel->second < 0 ||
                bufferMessage->transport != lcl::protocol::
                    LCLNativeBufferTransport::AndroidHardwareBufferV1) {
                if (msg.passedFd >= 0) close(msg.passedFd);
                requestAck.error(4, "AHB_V1 transport unavailable");
                continue;
            }

            AHardwareBuffer* hardwareBuffer = nullptr;
            if (AHardwareBuffer_recvHandleFromUnixSocket(
                    channel->second, &hardwareBuffer) != 0 || !hardwareBuffer) {
                if (msg.passedFd >= 0) close(msg.passedFd);
                requestAck.error(4, "AHB_V1 handle receive failed");
                continue;
            }
            if (msg.passedFd >= 0) {
                const int acquireFenceFd = msg.passedFd;
                if (!m_renderer.waitNativeFence(acquireFenceFd)) {
                    pollfd descriptor{acquireFenceFd, POLLIN, 0};
                    (void)poll(&descriptor, 1, 3000);
                    close(acquireFenceFd);
                }
            }

            AHardwareBuffer_Desc hardwareDesc{};
            AHardwareBuffer_describe(hardwareBuffer, &hardwareDesc);
            const uint64_t surfaceKey =
                (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) |
                bufferMessage->surfaceId;
            auto surfaceIt = m_surfaces.find(surfaceKey);
            const auto releaseRejected = [&] {
                lcl::protocol::LCLHeader releaseHeader{};
                releaseHeader.opcode = lcl::protocol::LCLOpcode::ReleaseDmaBuf;
                releaseHeader.payloadSize =
                    sizeof(lcl::protocol::LCLMsgReleaseDmaBuf);
                lcl::protocol::LCLMsgReleaseDmaBuf release{};
                release.surfaceId = bufferMessage->surfaceId;
                release.bufferId = bufferMessage->bufferId;
                lcl::protocol::sendMsgWithFd(
                    msg.clientFd, releaseHeader, &release);
            };
            if (surfaceIt == m_surfaces.end()) {
                AHardwareBuffer_release(hardwareBuffer);
                continue;
            }
            auto& entry = surfaceIt->second;
            if (hardwareDesc.width < bufferMessage->backingWidth ||
                hardwareDesc.height < bufferMessage->backingHeight ||
                entry.ignoreBufferCommits ||
                entry.transitionPhase == SurfaceEntry::TransitionPhase::Closing ||
                !SurfaceRegistry::acceptsBufferCommit(
                    entry, bufferMessage->configureSerial)) {
                AHardwareBuffer_release(hardwareBuffer);
                releaseRejected();
                continue;
            }

            lcl::platform::android::AHardwareNativeBuffer nativeBuffer(
                hardwareBuffer, false);
            const uint32_t texture =
                m_renderer.getRasterRenderer()->importTexture(nativeBuffer);
            AHardwareBuffer_release(hardwareBuffer);
            if (texture == 0) {
                releaseRejected();
                requestAck.error(4, "AHB_V1 import unavailable");
                continue;
            }

            if (entry.resizeTransitionPhase ==
                    SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer &&
                entry.hasRenderableBuffer()) {
                SurfaceRegistry::releasePreviousBuffer(entry);
                entry.previousPixels = entry.pixels;
                entry.previousShmSize = entry.shmSize;
                entry.previousShmFd = entry.shmFd;
                entry.previousWidth = entry.width;
                entry.previousHeight = entry.height;
                entry.previousBackingWidth = entry.backingWidth;
                entry.previousBackingHeight = entry.backingHeight;
                entry.previousStride = entry.stride;
                entry.previousShmContentSerial = entry.shmContentSerial;
                entry.previousDmaBufId = entry.dmaBufId;
                entry.previousDmaBufTexture = entry.dmaBufTexture;
                entry.pixels = nullptr;
                entry.shmSize = 0;
                entry.shmFd = -1;
                entry.shmContentSerial = 0;
                entry.dmaBufId = 0;
                entry.dmaBufTexture = 0;
            } else {
                if (entry.dmaBufTexture != 0 || entry.dmaBufId != 0) {
                    entry.pendingDmaBufReleases.push_back(
                        {entry.dmaBufId, entry.dmaBufTexture});
                }
                if (entry.pixels && entry.shmSize > 0)
                    munmap(entry.pixels, entry.shmSize);
                if (entry.shmFd >= 0) close(entry.shmFd);
                entry.pixels = nullptr;
                entry.shmSize = 0;
                entry.shmFd = -1;
                entry.shmContentSerial = 0;
                entry.dmaBufId = 0;
                entry.dmaBufTexture = 0;
            }
            entry.clientFd = msg.clientFd;
            entry.dmaBufId = bufferMessage->bufferId;
            entry.dmaBufTexture = texture;
            if (!entry.dmaBufTransportActive) {
                entry.dmaBufTransportActive = true;
                std::cerr << "[LCL AHB] Surface " << bufferMessage->surfaceId
                          << " zero-copy import active\n";
            }
            entry.width = bufferMessage->width;
            entry.height = bufferMessage->height;
            entry.backingWidth = bufferMessage->backingWidth;
            entry.backingHeight = bufferMessage->backingHeight;
            entry.stride = hardwareDesc.stride * 4u;
            entry.acceptedConfigureSerial = bufferMessage->configureSerial;
            if (entry.resizeTransitionPhase ==
                SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer) {
                entry.resizeTransitionPhase =
                    SurfaceEntry::ResizeTransitionPhase::Crossfading;
                entry.resizeCrossfadeElapsedSec = 0.0f;
                entry.resizeCrossfadeProgress = 0.0f;
            }
            if (entry.isPopup()) {
                entry.hasCommittedBuffer = true;
            } else if (!entry.hasCommittedBuffer &&
                       !mapSurface(surfaceKey, entry, msg.pid)) {
                continue;
            }
            if (entry.isPopup()) {
                SurfaceRegistry::queuePresentation(
                    entry, bufferMessage->configureSerial);
                changed = true;
                continue;
            }

            int titleOffset = 0;
            for (const auto& win : m_windowManager.getWindows()) {
                if (win.id == entry.windowId &&
                    win.decorationMode == render::DecorationMode::SSD) {
                    titleOffset = 32;
                    break;
                }
            }
            const float requestedContentW = entry.configuredWidth;
            const float requestedContentH = entry.configuredHeight;
            const float committedContentW =
                SurfaceRegistry::committedLogicalExtent(
                    entry.width, requestedContentW, entry.bufferScale);
            const float committedContentH =
                SurfaceRegistry::committedLogicalExtent(
                    entry.height, requestedContentH, entry.bufferScale);
            const float frameW = committedContentW;
            const float frameH = committedContentH +
                static_cast<float>(titleOffset);
            bool preserveNewerTarget = false;
            const auto configuredWindow = std::find_if(
                m_windowManager.getWindows().begin(),
                m_windowManager.getWindows().end(),
                [&entry](const auto& window) {
                    return window.id == entry.windowId;
                });
            if (configuredWindow != m_windowManager.getWindows().end()) {
                const float configuredFrameHeight = requestedContentH +
                    static_cast<float>(titleOffset);
                preserveNewerTarget =
                    configuredWindow->pendingWidth != requestedContentW ||
                    configuredWindow->pendingHeight != configuredFrameHeight;
            }
            entry.configuredWidth = committedContentW;
            entry.configuredHeight = committedContentH;
            m_windowManager.commitSurfaceGeometry(
                entry.windowId, frameW, frameH, preserveNewerTarget,
                entry.configuredX, entry.configuredY,
                entry.configuredGeometryGeneration);
            SurfaceRegistry::queuePresentation(
                entry, bufferMessage->configureSerial);
            changed = true;
#else
            if (msg.passedFd >= 0) close(msg.passedFd);
            requestAck.error(4, "AHB_V1 import unavailable");
            continue;
#endif

        // --- ATTACH_BUFFER: mmap the SCM_RIGHTS memfd into compositor address space ---
        } else if (isAttachBuffer) {
            uint32_t surfId = 1;
            uint32_t w = 540, h = 360, stride = 540 * 4;

            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgAttachBuffer)) {
                auto* bm = reinterpret_cast<const lcl::protocol::LCLMsgAttachBuffer*>(msg.payload.data());
                surfId = bm->surfaceId;
                w = bm->width;
                h = bm->height;
                stride = bm->stride > 0 ? bm->stride : w * 4;
            }

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            auto surfaceIt = m_surfaces.find(surfaceKey);
            if (surfaceIt == m_surfaces.end()) {
                if (msg.passedFd >= 0) {
                    close(msg.passedFd);
                }
                std::cerr << "[LCL Compositor ERROR] Ignoring AttachBuffer for unknown Surface "
                          << surfId << " from client PID " << msg.pid << "\n";
                continue;
            }

            auto& entry = surfaceIt->second;
            if (entry.ignoreBufferCommits || entry.transitionPhase == SurfaceEntry::TransitionPhase::Closing) {
                if (msg.passedFd >= 0) {
                    close(msg.passedFd);
                }
                continue;
            }

            const auto* bufferMessage = reinterpret_cast<const lcl::protocol::LCLMsgAttachBuffer*>(msg.payload.data());
            const auto discardRejected = [&] {
                lcl::protocol::LCLHeader discardHeader{};
                discardHeader.opcode = lcl::protocol::LCLOpcode::FrameDiscarded;
                discardHeader.payloadSize = sizeof(lcl::protocol::LCLMsgFrameDiscarded);
                lcl::protocol::LCLMsgFrameDiscarded discard{};
                discard.surfaceId = bufferMessage->surfaceId;
                discard.configureSerial = bufferMessage->configureSerial;
                lcl::protocol::sendMsgWithFd(msg.clientFd, discardHeader, &discard);
            };
            if (!SurfaceRegistry::acceptsBufferCommit(
                    entry, bufferMessage->configureSerial)) {
                if (msg.passedFd >= 0) close(msg.passedFd);
                std::cerr << "[LCL Compositor] Rejected stale buffer serial "
                          << bufferMessage->configureSerial << " (expected "
                          << entry.pendingConfigureSerial << ") for Surface " << surfId
                          << "; buffer=" << w << 'x' << h
                          << ", configured=" << entry.configuredWidth << 'x'
                          << entry.configuredHeight << "\n";
                discardRejected();
                continue;
            }

            entry.clientFd = msg.clientFd;

            int fd = msg.passedFd;
            bool bufferCommitAccepted = false;
            if (fd >= 0) {
                struct stat shmStat{};
                const size_t minimumSize = static_cast<size_t>(stride) * h;
                const bool validShmSize = fstat(fd, &shmStat) == 0 &&
                    shmStat.st_size > 0 &&
                    static_cast<uint64_t>(shmStat.st_size) >= minimumSize;
                const size_t shmSize = validShmSize
                    ? static_cast<size_t>(shmStat.st_size) : 0;
                if (!validShmSize || stride < w * sizeof(uint32_t)) {
                    std::cerr << "[LCL Compositor] Rejected undersized SHM backing for Surface "
                              << surfId << "\n";
                    close(fd);
                    discardRejected();
                    continue;
                }
                if (entry.pixels && entry.shmSize == shmSize &&
                    entry.stride == stride) {
                    // This retained-capacity mapping is already visible in the
                    // compositor. Only its active content extent changed.
                    close(fd);
                    entry.width = w;
                    entry.height = h;
                    bufferCommitAccepted = true;
                } else {
                    void* pixels = mmap(nullptr, shmSize, PROT_READ, MAP_SHARED, fd, 0);
                    if (pixels != MAP_FAILED) {
                        if (entry.resizeTransitionPhase == SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer &&
                            entry.hasRenderableBuffer()) {
                            SurfaceRegistry::releasePreviousBuffer(entry);
                            entry.previousPixels = entry.pixels;
                            entry.previousShmSize = entry.shmSize;
                            entry.previousShmFd = entry.shmFd;
                            entry.previousWidth = entry.width;
                            entry.previousHeight = entry.height;
                            entry.previousStride = entry.stride;
                            entry.previousShmContentSerial = entry.shmContentSerial;
                            entry.previousDmaBufId = entry.dmaBufId;
                            entry.previousDmaBufTexture = entry.dmaBufTexture;
                            entry.pixels = nullptr;
                            entry.shmSize = 0;
                            entry.shmFd = -1;
                            entry.dmaBufId = 0;
                            entry.dmaBufTexture = 0;
                        } else {
                            if (entry.dmaBufTexture != 0 || entry.dmaBufId != 0) {
                                entry.pendingDmaBufReleases.push_back({entry.dmaBufId, entry.dmaBufTexture});
                            }
                            entry.dmaBufId = 0;
                            entry.dmaBufTexture = 0;
                            if (entry.pixels && entry.shmSize > 0) munmap(entry.pixels, entry.shmSize);
                            if (entry.shmFd >= 0 && entry.shmFd != fd) close(entry.shmFd);
                        }
                        entry.pixels  = pixels;
                        entry.shmSize = shmSize;
                        entry.shmFd   = fd;
                        entry.width   = w;
                        entry.height  = h;
                        entry.backingWidth = stride / sizeof(uint32_t);
                        entry.backingHeight = static_cast<uint32_t>(shmSize / stride);
                        entry.stride  = stride;
                        bufferCommitAccepted = true;
                        if (entry.resizeTransitionPhase == SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer) {
                            entry.resizeTransitionPhase = SurfaceEntry::ResizeTransitionPhase::Crossfading;
                            entry.resizeCrossfadeElapsedSec = 0.0f;
                            entry.resizeCrossfadeProgress = 0.0f;
                        }
                    } else {
                        std::cerr << "[LCL Compositor ERROR] mmap failed for memfd " << fd
                                  << ": " << strerror(errno) << "\n";
                        close(fd);
                    }
                }
            } else {
                // A configure that resolves to the currently mapped size (for
                // example Terminal cell-grid snapping) needs no replacement
                // memfd. It still acknowledges the serial and releases
                // configure backpressure.
                if (!entry.pixels || entry.stride != stride ||
                    stride < w * sizeof(uint32_t) ||
                    static_cast<size_t>(stride) * h > entry.shmSize) {
                    std::cerr << "[LCL Compositor] Rejected buffer commit without matching memfd for Surface "
                              << surfId << "; buffer=" << w << 'x' << h
                              << ", mapped=" << entry.width << 'x' << entry.height << "\n";
                    discardRejected();
                    continue;
                }
                entry.width  = w;
                entry.height = h;
                entry.stride = stride;
                bufferCommitAccepted = true;
            }

            if (!bufferCommitAccepted) {
                discardRejected();
                continue;
            }
            entry.acceptedConfigureSerial = bufferMessage->configureSerial;
            entry.shmContentSerial = m_nextShmContentSerial++;
            if (m_nextShmContentSerial == 0) m_nextShmContentSerial = 1;
            const bool validDamage = bufferMessage->damageWidth > 0 &&
                bufferMessage->damageHeight > 0 &&
                bufferMessage->damageX < w && bufferMessage->damageY < h;
            if (!validDamage) {
                entry.shmDamageX = 0;
                entry.shmDamageY = 0;
                entry.shmDamageWidth = w;
                entry.shmDamageHeight = h;
            } else {
                const uint32_t damageRight = bufferMessage->damageX + std::min(
                    bufferMessage->damageWidth, w - bufferMessage->damageX);
                const uint32_t damageBottom = bufferMessage->damageY + std::min(
                    bufferMessage->damageHeight, h - bufferMessage->damageY);
                entry.shmDamageX = bufferMessage->damageX;
                entry.shmDamageY = bufferMessage->damageY;
                entry.shmDamageWidth = damageRight - bufferMessage->damageX;
                entry.shmDamageHeight = damageBottom - bufferMessage->damageY;
            }

            // Mapping is intentionally evaluated after every commit rather
            // than only in the SCM_RIGHTS branch.  A client can retain a
            // successfully mapped SHM buffer while retrying its first commit;
            // the surface must then become visible as soon as that buffer is
            // known to be valid.
            if (entry.isPopup()) {
                entry.hasCommittedBuffer = true;
            } else if (!entry.hasCommittedBuffer &&
                       !mapSurface(surfaceKey, entry, msg.pid)) {
                // The first visible commit must include a real shared buffer.
                continue;
            }

            if (entry.isPopup()) {
                SurfaceRegistry::queuePresentation(
                    entry, bufferMessage->configureSerial);
                changed = true;
                continue;
            }

            // Calculate titleOffset based on the mapped window decoration mode.
            int titleOffset = 0;
            for (const auto& win : m_windowManager.getWindows()) {
                if (win.id == entry.windowId) {
                    if (win.decorationMode == render::DecorationMode::SSD) {
                        titleOffset = 32;
                    }
                    break;
                }
            }

            // Notify WindowManager of client surface buffer commit
            const float requestedContentW = entry.configuredWidth;
            const float requestedContentH = entry.configuredHeight;
            const float committedContentW = SurfaceRegistry::committedLogicalExtent(
                entry.width, requestedContentW, entry.bufferScale);
            const float committedContentH = SurfaceRegistry::committedLogicalExtent(
                entry.height, requestedContentH, entry.bufferScale);
            const float frameW = committedContentW;
            const float frameH = committedContentH + static_cast<float>(titleOffset);
            bool preserveNewerTarget = false;
            const auto configuredWindow = std::find_if(
                m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
                [&entry](const auto& window) { return window.id == entry.windowId; });
            if (configuredWindow != m_windowManager.getWindows().end()) {
                const float configuredFrameHeight =
                    requestedContentH + static_cast<float>(titleOffset);
                preserveNewerTarget =
                    configuredWindow->pendingWidth != requestedContentW ||
                    configuredWindow->pendingHeight != configuredFrameHeight;
            }
            entry.configuredWidth = committedContentW;
            entry.configuredHeight = committedContentH;
            m_windowManager.commitSurfaceGeometry(
                entry.windowId, frameW, frameH, preserveNewerTarget,
                entry.configuredX, entry.configuredY,
                entry.configuredGeometryGeneration);
            SurfaceRegistry::queuePresentation(
                entry, bufferMessage->configureSerial);
            changed = true;

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

            const uint32_t windowId = surfaceIt->second.windowId;
            const auto currentWindow = std::find_if(
                m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
                [windowId](const auto& window) { return window.id == windowId; });
            const bool hasCurrentWindow = currentWindow != m_windowManager.getWindows().end();
            const float rollbackX = hasCurrentWindow ? currentWindow->x : 0.0f;
            const float rollbackY = hasCurrentWindow ? currentWindow->y : 0.0f;
            const float rollbackWidth = hasCurrentWindow ? currentWindow->width : 0.0f;
            const float rollbackHeight = hasCurrentWindow ? currentWindow->height : 0.0f;
            const bool rollbackWasMaximized = hasCurrentWindow && currentWindow->isMaximized;
            const bool rollbackWasMinimized = hasCurrentWindow && currentWindow->isMinimized;
            const bool compositorMorph = surfaceIt->second.resizePresentation ==
                protocol::LCLResizePresentationMode::CompositorMorph;
            const auto beginResizeTransition = [&] {
                if (!hasCurrentWindow || !compositorMorph) return;
                auto& entry = surfaceIt->second;
                const auto transitionedWindow = std::find_if(
                    m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(),
                    [windowId](const auto& window) { return window.id == windowId; });
                if (transitionedWindow == m_windowManager.getWindows().end()) return;
                SurfaceRegistry::beginGeometryTransition(
                    entry, transitionedWindow->geometryGeneration,
                    rollbackX, rollbackY, rollbackWidth, rollbackHeight,
                    rollbackWasMaximized, rollbackWasMinimized);
            };
            switch (request->action) {
                case lcl::protocol::LCLWindowAction::BeginDrag:
                    if (const auto interaction = m_windowManager.beginWindowDrag(
                        windowId, request->localX, request->localY)) {
                        SurfaceRegistry::interruptGeometryTransaction(
                            surfaceIt->second, interaction.generation);
                        changed = true;
                    }
                    break;
                case lcl::protocol::LCLWindowAction::Minimize:
                    if (windowId != 0 && surfaceIt->second.transitionPhase == SurfaceEntry::TransitionPhase::None) {
                        surfaceIt->second.transitionPhase = SurfaceEntry::TransitionPhase::Minimizing;
                        surfaceIt->second.transitionElapsedSec = 0.0f;
                        surfaceIt->second.transitionDurationSec = 0.18f;
                        surfaceIt->second.transitionOpacity = 1.0f;
                        surfaceIt->second.transitionScale = 1.0f;
                        m_windowManager.transferFocusFromWindow(windowId);
                        changed = true;
                    }
                    break;
                case lcl::protocol::LCLWindowAction::Maximize:
                    if (m_windowManager.maximizeWindow(windowId)) { beginResizeTransition(); changed = true; }
                    break;
                case lcl::protocol::LCLWindowAction::Restore:
                    if (hasCurrentWindow && currentWindow->isMinimized) {
                        if (m_windowManager.restoreWindow(windowId)) {
                            surfaceIt->second.transitionPhase = SurfaceEntry::TransitionPhase::Restoring;
                            surfaceIt->second.transitionElapsedSec = 0.0f;
                            surfaceIt->second.transitionDurationSec = 0.22f;
                            surfaceIt->second.transitionOpacity = 0.0f;
                            surfaceIt->second.transitionScale = 0.92f;
                            changed = true;
                        }
                    } else if (m_windowManager.restoreWindow(windowId)) { beginResizeTransition(); changed = true; }
                    break;
                case lcl::protocol::LCLWindowAction::ToggleMaximize:
                    if (m_windowManager.toggleMaximizeWindow(windowId)) { beginResizeTransition(); changed = true; }
                    break;
                case lcl::protocol::LCLWindowAction::Close:
                    requestSurfaceClose(surfaceKey, request->surfaceId);
                    break;
                default:
                    break;
            }

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
                        if (it->second.forceOpaque) {
                            it->second.effectRegions.clear();
                            continue;
                        }
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
