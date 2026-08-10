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
float sanitizeBufferScale(float scale) {
    return (std::isfinite(scale) && scale >= 0.5f && scale <= 4.0f) ? scale : 1.0f;
}
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
uint64_t fnv1aMix(uint64_t hash, uint8_t byte) {
    hash ^= static_cast<uint64_t>(byte);
    return hash * 1099511628211ull;
}
} // namespace

bool ProtocolDispatcher::process(IPCManager& ipcManager) {
    bool changed = false;
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

    for (const auto& msg : ipcManager.pollMessages()) {
        // --- SURFACE_CREATE: register a window on the compositor canvas ---
        bool isSurfaceCreate = (msg.header.opcode == lcl::protocol::LCLOpcode::SurfaceCreate) ||
                               (msg.command.rfind("SURFACE_CREATE:", 0) == 0);
        bool isAttachBuffer  = (msg.header.opcode == lcl::protocol::LCLOpcode::AttachBuffer) ||
                               (msg.command.rfind("ATTACH_BUFFER:", 0) == 0);

        bool isDisconnect   = (msg.header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) ||
                               (msg.command.rfind("CLIENT_DISCONNECT", 0) == 0);

        if (msg.header.opcode == lcl::protocol::LCLOpcode::RegisterRole) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgRegisterRole)) {
                auto* reg = reinterpret_cast<const lcl::protocol::LCLMsgRegisterRole*>(msg.payload.data());
                m_clientRoles[msg.clientFd] = reg->role;
            }
            continue;
        }

        if (isDisconnect) {
            m_clientRoles.erase(msg.clientFd);
            std::vector<uint64_t> surfacesToRemove;
            for (auto& [surfKey, entry] : m_surfaces) {
                if (entry.clientFd == msg.clientFd || (msg.pid > 0 && (surfKey >> 32) == static_cast<uint64_t>(msg.pid))) {
                    entry.clientFd = -1;
                    if (beginClosingTransition(entry)) {
                        if (entry.windowId > 0) {
                            m_windowManager.removeWindow(entry.windowId);
                        }
                        surfacesToRemove.push_back(surfKey);
                    }
                }
            }
            for (uint64_t key : surfacesToRemove) {
                m_surfaces.erase(key);
            }
            changed = true;
        } else if (isSurfaceCreate) {
            uint32_t surfId = 1;
            std::string title = "LCL Terminal";
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
            const protocol::LCLRole clientRole =
                (roleIt != m_clientRoles.end()) ? roleIt->second : protocol::LCLRole::ClientApp;
            const bool isWallpaper = clientRole == protocol::LCLRole::DesktopWallpaper;
            const bool isShellPanel = clientRole == protocol::LCLRole::ShellPanel;

            constexpr size_t kSurfaceCreateV1Size = offsetof(lcl::protocol::LCLMsgSurfaceCreate, bufferScale);
            if (msg.payload.size() >= kSurfaceCreateV1Size) {
                auto* sm = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceCreate*>(msg.payload.data());
                surfId = sm->surfaceId;
                if (sm->title[0]) title = sm->title;
                if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSurfaceCreate)) {
                    bufferScale = sanitizeBufferScale(sm->bufferScale);
                }
                winX = logicalToPhysical(sm->x, bufferScale);
                winY = logicalToPhysical(sm->y, bufferScale);
                winW = (sm->width > 0) ? logicalToPhysical(static_cast<int>(sm->width), bufferScale) : static_cast<int>(m_renderer.getWidth());
                winH = (sm->height > 0) ? logicalToPhysical(static_cast<int>(sm->height), bufferScale) : static_cast<int>(m_renderer.getHeight());
            }

            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                SurfaceEntry entry{};
                int frameW = winW;
                int frameH = winH + DisplayScale::titleBarHeight();
                entry.windowId = m_windowManager.createWindow(
                    title, winX, winY, frameW, frameH,
                    ::lcl::theme::UI::WindowTitleFocused);
                if (isWallpaper || isShellPanel) {
                    const auto layer = isWallpaper
                        ? protocol::LCLWindowLayer::Bottom
                        : protocol::LCLWindowLayer::TopMost;
                    m_windowManager.setDecorationMode(entry.windowId, render::DecorationMode::None);
                    m_windowManager.setWindowLayer(entry.windowId, layer, true);
                    m_windowManager.setInsetBorderEnabled(entry.windowId, false);
                }
                entry.clientFd = msg.clientFd;
                entry.width  = static_cast<uint32_t>(winW);
                entry.height = static_cast<uint32_t>(winH);
                entry.stride = entry.width * 4;
                entry.bufferScale = bufferScale;
                entry.appId = inferAppIdFromPid(msg.pid);
                m_surfaces[surfaceKey] = entry;
                std::cout << "[LCL Compositor] Created Window (ID: " << entry.windowId
                          << ") for Surface " << surfId
                          << " from client PID " << msg.pid << " (" << winW << "x" << winH << ")\n";
                changed = true;
            } else {
                m_surfaces[surfaceKey].clientFd = msg.clientFd;
            }

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
            if (m_surfaces.find(surfaceKey) == m_surfaces.end()) {
                static int spawnIndex = 0;
                int winX = DisplayScale::px(80 + (spawnIndex % 6) * 30);
                int winY = DisplayScale::px(60 + (spawnIndex % 6) * 30);
                spawnIndex++;

                SurfaceEntry entry{};
                int frameW = static_cast<int>(w);
                int frameH = static_cast<int>(h) + DisplayScale::titleBarHeight();
                entry.windowId = m_windowManager.createWindow(
                    "LCL Terminal", winX, winY, frameW, frameH,
                    ::lcl::theme::UI::WindowTitleFocused);
                entry.clientFd = msg.clientFd;
                entry.width  = w;
                entry.height = h;
                entry.stride = stride;
                entry.bufferScale = 1.0f;
                entry.appId = inferAppIdFromPid(msg.pid);
                m_surfaces[surfaceKey] = entry;
            }

            auto& entry = m_surfaces[surfaceKey];
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
                        if (!entry.hasCommittedBuffer) {
                            entry.hasCommittedBuffer = true;
                            entry.transitionPhase = SurfaceEntry::TransitionPhase::Entering;
                            entry.transitionElapsedSec = 0.0f;
                            entry.transitionDurationSec = 0.20f;
                            entry.transitionOpacity = 0.0f;
                            entry.transitionScale = 0.96f;
                        }
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

            // Calculate titleOffset based on window decoration mode
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

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetDecorationMode ||
                   msg.command.rfind("SET_DECORATION_MODE", 0) == 0) {
            uint32_t surfId = 1;
            lcl::protocol::LCLDecorationMode mode = lcl::protocol::LCLDecorationMode::SSD;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetDecorationMode)) {
                auto* decMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetDecorationMode*>(msg.payload.data());
                surfId = decMsg->surfaceId;
                mode = decMsg->mode;
            } else if (msg.command.find("CSD") != std::string::npos) {
                mode = lcl::protocol::LCLDecorationMode::CSD;
            } else if (msg.command.find("NONE") != std::string::npos || msg.command.find("FRAMELESS") != std::string::npos) {
                mode = lcl::protocol::LCLDecorationMode::None;
            }
            uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
            auto it = m_surfaces.find(surfaceKey);
            if (it != m_surfaces.end()) {
                render::DecorationMode wmMode = render::DecorationMode::SSD;
                if (mode == lcl::protocol::LCLDecorationMode::CSD) {
                    wmMode = render::DecorationMode::CSD;
                } else if (mode == lcl::protocol::LCLDecorationMode::None) {
                    wmMode = render::DecorationMode::None;
                }
                m_windowManager.setDecorationMode(it->second.windowId, wmMode);
                std::cout << "[LCL Compositor] Set decoration mode for Window " << it->second.windowId
                          << " to " << (wmMode == render::DecorationMode::None ? "None (Frameless)" : (wmMode == render::DecorationMode::CSD ? "CSD" : "SSD")) << "\n";
                changed = true;
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::BeginWindowMove) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgBeginWindowMove)) {
                auto* moveMsg = reinterpret_cast<const lcl::protocol::LCLMsgBeginWindowMove*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | moveMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end() && it->second.windowId > 0) {
                    const uint32_t targetWinId = it->second.windowId;
                    m_windowManager.focusWindow(targetWinId);
                    auto& windows = m_windowManager.getWindowsMutable();
                    auto winIt = std::find_if(windows.begin(), windows.end(), [targetWinId](const render::Window& w) {
                        return w.id == targetWinId;
                    });
                    if (winIt != windows.end()) {
                        winIt->isDragging = true;
                        winIt->dragOffsetX = logicalToPhysical(static_cast<int>(std::lround(moveMsg->localX)), it->second.bufferScale);
                        winIt->dragOffsetY = logicalToPhysical(static_cast<int>(std::lround(moveMsg->localY)), it->second.bufferScale);
                        winIt->markDirty();
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
            auto it = m_surfaces.find(surfaceKey);
            if (it != m_surfaces.end()) {
                // Ask client to stop its loop while compositor animates frozen last frame.
                if (it->second.clientFd >= 0) {
                    lcl::protocol::LCLHeader destroyHeader{};
                    destroyHeader.opcode = lcl::protocol::LCLOpcode::SurfaceDestroy;
                    destroyHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceDestroy);
                    lcl::protocol::LCLMsgSurfaceDestroy destroyMsg{};
                    destroyMsg.surfaceId = surfId;
                    lcl::protocol::sendMsgWithFd(it->second.clientFd, destroyHeader, &destroyMsg);
                }

                if (beginClosingTransition(it->second)) {
                    if (it->second.windowId > 0) {
                        m_windowManager.removeWindow(it->second.windowId);
                    }
                    m_surfaces.erase(it);
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowLayer) {
            uint32_t surfId = 1;
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowLayer)) {
                auto* layerMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowLayer*>(msg.payload.data());
                surfId = layerMsg->surfaceId;
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | surfId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    m_windowManager.setWindowLayer(it->second.windowId, layerMsg->layer, layerMsg->unfocusable != 0);

                    // Default policy: wallpaper and unfocusable top overlays (menu bar) do not get forced inset borders.
                    const bool disableInsetBorder =
                        (layerMsg->layer == lcl::protocol::LCLWindowLayer::Bottom) ||
                        (layerMsg->layer == lcl::protocol::LCLWindowLayer::TopMost && layerMsg->unfocusable != 0);
                    m_windowManager.setInsetBorderEnabled(it->second.windowId, !disableInsetBorder);

                    std::cout << "[LCL Compositor] Set window layer for Window " << it->second.windowId
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
                    m_windowManager.setInsetBorderEnabled(it->second.windowId, borderMsg->enabled != 0);
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowCornerRadius) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowCornerRadius)) {
                auto* radiusMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerRadius*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | radiusMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    m_windowManager.setWindowCornerRadius(it->second.windowId, radiusMsg->radiusPx * it->second.bufferScale);
                    changed = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetReservedZone) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetReservedZone)) {
                auto* resMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetReservedZone*>(msg.payload.data());
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

        } else if (msg.command == "SPAWN_TERMINAL" || msg.command.rfind("SPAWN_TERMINAL", 0) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                // Child process: execute lcl-terminal binary
                execl("/home/user/Applications/Terminal.app/bin/lcl-terminal", "lcl-terminal", nullptr);
                execl("/bin/lcl-terminal", "lcl-terminal", nullptr);
                execl("/usr/bin/lcl-terminal", "lcl-terminal", nullptr);
                _exit(1);
            } else if (pid > 0) {
                std::cout << "[LCL Compositor] Spawned new LCL Terminal process (PID: " << pid << ")\n";
            } else {
                std::cerr << "[LCL Compositor ERROR] Failed to fork process for SPAWN_TERMINAL.\n";
            }
        }
    }

    publishWindowListToShellClients();
    return changed;
}

void ProtocolDispatcher::publishWindowListToShellClients() {
    std::vector<protocol::LCLMsgWindowListEntry> entries;
    entries.reserve(m_windowManager.getWindows().size());

    uint64_t hash = 1469598103934665603ull;
    for (const auto& win : m_windowManager.getWindows()) {
        if (win.layer == protocol::LCLWindowLayer::Bottom) continue;
        if (win.layer == protocol::LCLWindowLayer::TopMost && win.isUnfocusable) continue;

        protocol::LCLMsgWindowListEntry e{};
        e.windowId = win.id;
        e.isFocused = win.isFocused ? 1 : 0;
        std::strncpy(e.title, win.title.c_str(), sizeof(e.title) - 1);

        for (const auto& [_, surf] : m_surfaces) {
            if (surf.windowId == win.id && !surf.appId.empty()) {
                std::strncpy(e.appId, surf.appId.c_str(), sizeof(e.appId) - 1);
                break;
            }
        }

        hash = fnv1aMix(hash, static_cast<uint8_t>(e.windowId & 0xFF));
        hash = fnv1aMix(hash, static_cast<uint8_t>((e.windowId >> 8) & 0xFF));
        hash = fnv1aMix(hash, static_cast<uint8_t>((e.windowId >> 16) & 0xFF));
        hash = fnv1aMix(hash, static_cast<uint8_t>((e.windowId >> 24) & 0xFF));
        hash = fnv1aMix(hash, e.isFocused);
        for (char c : e.title) {
            if (c == '\0') break;
            hash = fnv1aMix(hash, static_cast<uint8_t>(c));
        }
        hash = fnv1aMix(hash, 0xFF);
        for (char c : e.appId) {
            if (c == '\0') break;
            hash = fnv1aMix(hash, static_cast<uint8_t>(c));
        }
        entries.push_back(e);
    }

    if (hash == m_lastWindowListHash) {
        return;
    }
    m_lastWindowListHash = hash;

    protocol::LCLMsgWindowListHeader listHeader{};
    listHeader.windowCount = static_cast<uint32_t>(entries.size());
    std::vector<uint8_t> payload(sizeof(listHeader) + entries.size() * sizeof(protocol::LCLMsgWindowListEntry));
    std::memcpy(payload.data(), &listHeader, sizeof(listHeader));
    if (!entries.empty()) {
        std::memcpy(payload.data() + sizeof(listHeader), entries.data(), entries.size() * sizeof(protocol::LCLMsgWindowListEntry));
    }

    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::WindowListUpdate;
    header.payloadSize = static_cast<uint32_t>(payload.size());

    for (const auto& [fd, role] : m_clientRoles) {
        if (role == protocol::LCLRole::DesktopWallpaper || role == protocol::LCLRole::ShellPanel) {
            protocol::sendMsgWithFd(fd, header, payload.data());
        }
    }
}

} // namespace lcl::core
