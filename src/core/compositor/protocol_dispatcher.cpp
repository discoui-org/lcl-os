#include "core/compositor/protocol_dispatcher.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

namespace lcl::core {
namespace {
int logicalToPhysical(int value, float scale) {
    return static_cast<int>(std::lround(static_cast<float>(value) * scale));
}
uint32_t physicalToLogical(uint32_t value, float scale) {
    return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(value) / scale)));
}
std::string inferAppIdFromPid(pid_t pid) {
    if (pid <= 0) return "";
    std::array<char, 64> procPath{};
    std::snprintf(procPath.data(), procPath.size(), "/proc/%d/exe", static_cast<int>(pid));
    std::array<char, 4096> resolved{};
    const ssize_t count = readlink(procPath.data(), resolved.data(), resolved.size() - 1);
    if (count <= 0) return "";
    resolved[static_cast<size_t>(count)] = '\0';
    std::filesystem::path executable(resolved.data());
    for (auto current = executable; !current.empty(); current = current.parent_path()) {
        if (current.extension() == ".app") return current.stem().string();
        if (current == current.root_path()) break;
    }
    return executable.stem().string();
}
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
        if (entry.windowId != 0 || !entry.pixels || entry.width == 0 || entry.height == 0) {
            return false;
        }

        const auto decorationMode = toRenderDecorationMode(entry.decorationMode);
        const int titleOffset = decorationMode == render::DecorationMode::SSD
            ? DisplayScale::titleBarHeight()
            : 0;
        entry.windowId = m_windowManager.createWindow(
            entry.title, entry.initialX, entry.initialY,
            static_cast<int>(entry.width), static_cast<int>(entry.height) + titleOffset,
            ::lcl::theme::UI::WindowTitleFocused, !entry.unfocusable);
        m_windowManager.setDecorationMode(entry.windowId, decorationMode);
        m_windowManager.setWindowLayer(entry.windowId, entry.layer, entry.unfocusable);
        m_windowManager.setInsetBorderEnabled(entry.windowId, entry.insetBorderEnabled);
        if (entry.cornerRadiusPx >= 0.0f) {
            m_windowManager.setWindowCornerRadius(entry.windowId, entry.cornerRadiusPx);
        }

        if (entry.suppressInitialTransition) {
            entry.transitionPhase = SurfaceEntry::TransitionPhase::None;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
        } else {
            entry.transitionPhase = SurfaceEntry::TransitionPhase::Entering;
            entry.transitionElapsedSec = 0.0f;
            entry.transitionDurationSec = 0.20f;
            entry.transitionOpacity = 0.0f;
            entry.transitionScale = 0.96f;
        }
        entry.hasCommittedBuffer = true;
        if (entry.role == protocol::LCLRole::ClientApp) {
            m_scenes.mapClientSurface(surfaceKey, clientPid, entry.windowId, entry.appId, entry.title);
        }
        std::cout << "[LCL Compositor] Mapped Window (ID: " << entry.windowId
                  << ") after first client buffer commit\n";
        return true;
    };
    auto beginClosingTransition = [&](SurfaceEntry& entry) {
        entry.ignoreBufferCommits = true;
        if (entry.windowId > 0 && entry.pixels && entry.width > 0 && entry.height > 0) {
            entry.transitionPhase = SurfaceEntry::TransitionPhase::Closing;
            entry.transitionElapsedSec = 0.0f;
            entry.transitionDurationSec = 0.14f;
            entry.transitionOpacity = 1.0f;
            entry.transitionScale = 1.0f;
            entry.pendingDestroy = false;
            std::cout << "[LCL Compositor] Closing transition started for Window ID: "
                      << entry.windowId << "\n";
            changed = true;
            return false;
        }
        return true;
    };

    auto requestSurfaceClose = [&](uint64_t surfaceKey, uint32_t surfaceId) {
        auto it = m_surfaces.find(surfaceKey);
        if (it == m_surfaces.end()) return;

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

        if (beginClosingTransition(it->second)) {
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
            m_clientRoles.erase(msg.clientFd);
            m_pendingSystemSurfaceKinds.erase(msg.clientFd);
            m_shellSubscriptions.erase(msg.clientFd);
            std::vector<uint64_t> surfacesToRemove;
            for (auto& [surfKey, entry] : m_surfaces) {
                if (entry.clientFd == msg.clientFd ||
                    (msg.pid > 0 && (surfKey >> 32) == static_cast<uint64_t>(msg.pid))) {
                    entry.clientFd = -1;
                    if (beginClosingTransition(entry)) {
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
        const bool isAttachBuffer = msg.header.opcode == lcl::protocol::LCLOpcode::AttachBuffer;

        if (msg.header.opcode == lcl::protocol::LCLOpcode::RegisterRole) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgRegisterRole)) {
                auto* reg = reinterpret_cast<const lcl::protocol::LCLMsgRegisterRole*>(msg.payload.data());
                if ((reg->role == protocol::LCLRole::ShellPanel ||
                     reg->role == protocol::LCLRole::DesktopWallpaper) &&
                    !SystemSurfacePolicyRegistry::isTrustedShellPeer(msg.pid)) {
                    requestAck.error(5, "privileged shell role denied");
                    continue;
                }
                m_clientRoles[msg.clientFd] = reg->role;
            }
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
            const auto role = m_clientRoles.find(msg.clientFd);
            if (role == m_clientRoles.end() || role->second != protocol::LCLRole::ShellPanel) {
                requestAck.error(4, "shell-state subscription requires ShellPanel role");
                continue;
            }
            const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgSubscribeShellState*>(msg.payload.data());
            m_shellSubscriptions[msg.clientFd] = ShellSubscription{request->lastKnownRevision, false};
            changed = true;
            continue;
        }

        if (isSurfaceCreate) {
            uint32_t surfId = 1;
            std::string title = "LCL Application";
            int winX = DisplayScale::px(80);
            int winY = DisplayScale::px(60);
            int winW = DisplayScale::px(540);
            int winH = DisplayScale::px(360);
            float bufferScale = 1.0f;

            // RegisterRole is delivered on the same ordered stream before
            // SurfaceCreate.  Apply shell roles at creation time, rather than
            // briefly creating panels as ordinary decorated windows and waiting
            // for a later SetDecorationMode/SetWindowLayer pair.
            const auto roleIt = m_clientRoles.find(msg.clientFd);
            protocol::LCLRole clientRole =
                (roleIt != m_clientRoles.end()) ? roleIt->second : protocol::LCLRole::ClientApp;

            if (msg.payload.size() == sizeof(lcl::protocol::LCLMsgSurfaceCreate)) {
                auto* sm = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data());
                surfId = sm->surfaceId;
                if (sm->title[0]) title = sm->title;
                bufferScale = sm->bufferScale;
                winX = logicalToPhysical(sm->x, bufferScale);
                winY = logicalToPhysical(sm->y, bufferScale);
                winW = (sm->width > 0) ? logicalToPhysical(static_cast<int>(sm->width), bufferScale) : static_cast<int>(m_renderer.getWidth());
                winH = (sm->height > 0) ? logicalToPhysical(static_cast<int>(sm->height), bufferScale) : static_cast<int>(m_renderer.getHeight());
            }

            auto kindIt = m_pendingSystemSurfaceKinds.find(msg.clientFd);
            const protocol::LCLSystemSurfaceKind requestedSystemKind =
                kindIt != m_pendingSystemSurfaceKinds.end()
                    ? kindIt->second
                    : SystemSurfacePolicyRegistry::inferLegacyKind(clientRole, title.c_str());
            const auto systemPolicy = SystemSurfacePolicyRegistry::policyFor(requestedSystemKind);
            if (systemPolicy.isSystemSurface) clientRole = systemPolicy.role;

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                SurfaceEntry entry{};
                entry.title = title;
                entry.initialX = winX;
                entry.initialY = winY;
                entry.initialWidth = static_cast<uint32_t>(winW);
                entry.initialHeight = static_cast<uint32_t>(winH);
                entry.role = clientRole;
                entry.systemSurfaceKind = requestedSystemKind;
                entry.suppressInitialTransition = systemPolicy.suppressInitialTransition;
                if (systemPolicy.isSystemSurface) {
                    entry.decorationMode = protocol::LCLDecorationMode::None;
                    entry.layer = systemPolicy.layer;
                    entry.unfocusable = systemPolicy.unfocusable;
                    entry.insetBorderEnabled = systemPolicy.insetBorderEnabled;
                }
                entry.clientFd = msg.clientFd;
                entry.bufferScale = bufferScale;
                entry.appId = (msg.payload.size() == sizeof(lcl::protocol::LCLMsgSurfaceCreate) &&
                               reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data())->appId[0] != '\0')
                    ? reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data())->appId
                    : inferAppIdFromPid(msg.pid);
                m_surfaces[surfaceKey] = entry;
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
            cfgMsg.x = logicalToPhysical(winX, 1.0f / bufferScale);
            cfgMsg.y = logicalToPhysical(winY, 1.0f / bufferScale);
            cfgMsg.width = physicalToLogical(static_cast<uint32_t>(winW), bufferScale);
            cfgMsg.height = physicalToLogical(static_cast<uint32_t>(winH), bufferScale);
            cfgMsg.bufferScale = bufferScale;
            cfgMsg.isFocused = 1;

            protocol::sendMsgWithFd(msg.clientFd, header, &cfgMsg);

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

            entry.clientFd = msg.clientFd;

            int fd = msg.passedFd;
            if (fd >= 0) {
                size_t shmSize = static_cast<size_t>(stride) * h;
                if (entry.pixels && entry.shmSize == shmSize && entry.width == w && entry.height == h) {
                    // Buffer is ALREADY mapped in compositor address space with identical size & dimensions!
                    // Do NOT unmap/remap memory on every frame to avoid rendering race conditions.
                    close(fd);
                } else {
                    void* pixels = mmap(nullptr, shmSize, PROT_READ, MAP_SHARED, fd, 0);
                    if (pixels != MAP_FAILED) {
                        if (entry.pixels && entry.shmSize > 0) {
                            munmap(entry.pixels, entry.shmSize);
                        }
                        if (entry.shmFd >= 0 && entry.shmFd != fd) {
                            close(entry.shmFd);
                        }
                        entry.pixels  = pixels;
                        entry.shmSize = shmSize;
                        entry.shmFd   = fd;
                        entry.width   = w;
                        entry.height  = h;
                        entry.stride  = stride;
                    } else {
                        std::cerr << "[LCL Compositor ERROR] mmap failed for memfd " << fd
                                  << ": " << strerror(errno) << "\n";
                        close(fd);
                    }
                }
            } else {
                entry.width  = w;
                entry.height = h;
                entry.stride = stride;
            }

            // Mapping is intentionally evaluated after every commit rather
            // than only in the SCM_RIGHTS branch.  A client can retain a
            // successfully mapped SHM buffer while retrying its first commit;
            // the surface must then become visible as soon as that buffer is
            // known to be valid.
            if (entry.windowId == 0 && !mapSurface(surfaceKey, entry, msg.pid)) {
                // The first visible commit must include a real shared buffer.
                continue;
            }

            // Calculate titleOffset based on the mapped window decoration mode.
            int titleOffset = 0;
            for (const auto& win : m_windowManager.getWindows()) {
                if (win.id == entry.windowId) {
                    if (win.decorationMode == render::DecorationMode::SSD) {
                        titleOffset = DisplayScale::titleBarHeight();
                    }
                    break;
                }
            }

            // Notify WindowManager of client surface buffer commit
            int frameW = static_cast<int>(w);
            int frameH = static_cast<int>(h) + titleOffset;
            m_windowManager.commitSurfaceGeometry(entry.windowId, frameW, frameH);
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
                it->second.decorationMode = mode;
                const auto wmMode = toRenderDecorationMode(mode);
                if (it->second.windowId != 0) {
                    m_windowManager.setDecorationMode(it->second.windowId, wmMode);
                }
                std::cout << "[LCL Compositor] Set decoration mode for Surface " << surfId
                          << " to " << (wmMode == render::DecorationMode::None ? "None (Frameless)" : (wmMode == render::DecorationMode::CSD ? "CSD" : "SSD")) << "\n";
                changed = true;
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::RequestWindowAction) {
            if (msg.payload.size() < sizeof(lcl::protocol::LCLMsgRequestWindowAction)) {
                continue;
            }

            const auto* request = reinterpret_cast<const lcl::protocol::LCLMsgRequestWindowAction*>(msg.payload.data());
            const uint64_t surfaceKey =
                (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | request->surfaceId;
            const auto surfaceIt = m_surfaces.find(surfaceKey);
            if (surfaceIt == m_surfaces.end()) {
                continue;
            }

            const uint32_t windowId = surfaceIt->second.windowId;
            switch (request->action) {
                case lcl::protocol::LCLWindowAction::BeginDrag:
                    changed = m_windowManager.beginWindowDrag(
                        windowId,
                        logicalToPhysical(static_cast<int>(std::lround(request->localX)), surfaceIt->second.bufferScale),
                        logicalToPhysical(static_cast<int>(std::lround(request->localY)), surfaceIt->second.bufferScale)) || changed;
                    break;
                case lcl::protocol::LCLWindowAction::Minimize:
                    changed = m_windowManager.minimizeWindow(windowId) || changed;
                    break;
                case lcl::protocol::LCLWindowAction::Maximize:
                    changed = m_windowManager.maximizeWindow(windowId) || changed;
                    break;
                case lcl::protocol::LCLWindowAction::Restore:
                    changed = m_windowManager.restoreWindow(windowId) || changed;
                    break;
                case lcl::protocol::LCLWindowAction::ToggleMaximize:
                    changed = m_windowManager.toggleMaximizeWindow(windowId) || changed;
                    break;
                case lcl::protocol::LCLWindowAction::Close:
                    requestSurfaceClose(surfaceKey, request->surfaceId);
                    break;
                default:
                    break;
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::BeginWindowMove) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgBeginWindowMove)) {
                auto* moveMsg = reinterpret_cast<const lcl::protocol::LCLMsgBeginWindowMove*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | moveMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end() && it->second.windowId > 0) {
                    changed = m_windowManager.beginWindowDrag(
                        it->second.windowId,
                        logicalToPhysical(static_cast<int>(std::lround(moveMsg->localX)), it->second.bufferScale),
                        logicalToPhysical(static_cast<int>(std::lround(moveMsg->localY)), it->second.bufferScale)) || changed;
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
                    it->second.insetBorderEnabled = !disableInsetBorder;
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
                    it->second.insetBorderEnabled = borderMsg->enabled != 0;
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
                    it->second.cornerRadiusPx = radiusMsg->radiusPx * it->second.bufferScale;
                    if (it->second.windowId != 0) {
                        m_windowManager.setWindowCornerRadius(it->second.windowId, it->second.cornerRadiusPx);
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
                            if (offset + count > static_cast<size_t>(graphHeader->filterCount)) {
                                valid = false;
                                break;
                            }

                            SurfaceEffectRegion dstRegion;
                            dstRegion.region = region;
                            dstRegion.filters.assign(filters + offset, filters + offset + count);
                            dstRegion.followSurfaceBounds =
                                (region.x == 0 && region.y == 0 &&
                                 region.width == it->second.width &&
                                 region.height == it->second.height);
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
    uint32_t top = 0;
    uint32_t bottom = 0;
    for (const auto& [_, surface] : m_surfaces) {
        if (surface.windowId == 0) continue;
        if (surface.systemSurfaceKind == protocol::LCLSystemSurfaceKind::MenuBar) {
            top = std::max(top, surface.height);
        } else if (surface.systemSurfaceKind == protocol::LCLSystemSurfaceKind::Dock) {
            bottom = std::max(bottom, surface.height);
        }
    }
    m_windowManager.setReservedZone(top, bottom, 0, 0);
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
