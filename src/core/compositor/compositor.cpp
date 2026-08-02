#include "core/compositor/compositor.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"
#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/window_chrome.hpp"

#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <array>
#include <sys/mman.h>
#include <csignal>
#include <unistd.h>

namespace lcl::core {

namespace {

float easeOutCubic(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - x, 3.0f);
}

float easeInCubic(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * x;
}

uint8_t applyOpacityToAlpha(uint8_t alpha, float opacity) {
    const float scaled = std::clamp(static_cast<float>(alpha) * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 255.0f);
    return static_cast<uint8_t>(std::lround(scaled));
}

float sanitizeBufferScale(float scale) {
    if (!std::isfinite(scale) || scale < 0.5f || scale > 4.0f) {
        return 1.0f;
    }
    return scale;
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
    ssize_t n = readlink(procPath.data(), resolved.data(), resolved.size() - 1);
    if (n <= 0) return "";
    resolved[static_cast<size_t>(n)] = '\0';

    std::filesystem::path exePath(resolved.data());
    for (auto cur = exePath; !cur.empty(); cur = cur.parent_path()) {
        if (cur.extension() == ".app") {
            return cur.stem().string();
        }
        if (cur == cur.root_path()) {
            break;
        }
    }

    return exePath.stem().string();
}

uint64_t fnv1aMix(uint64_t h, uint8_t byte) {
    h ^= static_cast<uint64_t>(byte);
    h *= 1099511628211ull;
    return h;
}

} // namespace

// ============================================================
// Construction / Destruction
// ============================================================

Compositor::Compositor() = default;

Compositor::~Compositor() {
    if (!m_initialized) return;

    // 1. IPC — close socket before renderer/input tear-down
    m_ipcManager.shutdown();

    // 2. Renderer — release DRM framebuffer
    m_renderer.shutdown();

    // 3. Input — close evdev/libinput handles
    m_inputManager.shutdown();

    // 4. Display — release DRM/KMS lease
    m_displayManager.shutdown();

    std::cout << "[LCL Core] Clean shutdown complete. Total event loop ticks: "
              << m_loopTicks << "\n";
}

// ============================================================
// Initialization
// ============================================================

bool Compositor::initialize() {
    if (m_initialized) return true;

    signal(SIGPIPE, SIG_IGN);

    std::cout << "====================================================\n"
              << "  LCL Core Linux (LCL) v0.1.0 - Core Engine\n"
              << "  Architecture: Direct DRM/KMS & evdev (No X11/Wayland)\n"
              << "  C++ Standard: C++20\n"
              << "====================================================\n"
              << "[LCL Core] Initializing pure Display Server compositor...\n";

    // --- Display scale (reads lcl.scale= from /proc/cmdline) ---
    DisplayScale::initialize();

    // --- DRM/KMS Display ---
    if (!m_displayManager.initialize("/dev/dri/card0")) {
        std::cout << "[LCL Core] Display subsystem running in fallback/skeleton mode.\n";
    }

    // --- Input (libinput → evdev fallback) ---
    if (!m_inputManager.initialize("seat0")) {
        std::cout << "[LCL Core] Input subsystem running in fallback/skeleton mode.\n";
    }

    // --- Renderer ---
    if (!m_renderer.initialize(&m_displayManager)) {
        std::cout << "[LCL Core] Renderer running in fallback mode.\n";
    }

    // --- Window Manager Canvas ---
    m_windowManager.initialize(m_renderer.getWidth(), m_renderer.getHeight());

    // --- IPC (Unix Domain Socket, SO_PEERCRED auth, 0600 perms) ---
    m_ipcManager.initialize(kCompositorSocket);

    // --- Wire input events → WindowManager & focused IPC client ---
    m_inputManager.setEventCallback([this](const InputEvent& ev) {
        bool stateChanged = m_windowManager.processInputEvent(ev);
        if (stateChanged) {
            m_needsRedraw = true;

            // Notify surface clients if window bounds changed during resize
            for (const auto& win : m_windowManager.getWindows()) {
                for (const auto& [surfKey, entry] : m_surfaces) {
                    if (entry.windowId == win.id && entry.clientFd >= 0) {
                        int titleOffset = (win.decorationMode == render::DecorationMode::SSD) ? DisplayScale::titleBarHeight() : 0;
                        uint32_t physicalContentW = static_cast<uint32_t>(win.pendingWidth > 0 ? win.pendingWidth : win.width);
                        uint32_t physicalContentH = static_cast<uint32_t>(std::max(1, (win.pendingHeight > 0 ? win.pendingHeight : win.height) - titleOffset));
                        uint32_t contentW = physicalToLogical(physicalContentW, entry.bufferScale);
                        uint32_t contentH = physicalToLogical(physicalContentH, entry.bufferScale);
                        if (physicalContentW != entry.width || physicalContentH != entry.height) {
                            protocol::LCLHeader header{};
                            header.opcode = protocol::LCLOpcode::ConfigureBounds;
                            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

                            protocol::LCLMsgConfigureBounds cfgMsg{};
                            cfgMsg.surfaceId = static_cast<uint32_t>(surfKey & 0xFFFFFFFF);
                            cfgMsg.x = logicalToPhysical(win.x, 1.0f / entry.bufferScale);
                            cfgMsg.y = logicalToPhysical(win.y, 1.0f / entry.bufferScale);
                            cfgMsg.width = contentW;
                            cfgMsg.height = contentH;
                            cfgMsg.bufferScale = entry.bufferScale;
                            cfgMsg.isFocused = win.isFocused ? 1 : 0;

                            protocol::sendMsgWithFd(entry.clientFd, header, &cfgMsg);
                        }
                        break;
                    }
                }
            }

            std::vector<uint64_t> surfacesToRemove;
            std::vector<uint32_t> closeRequests;
            for (const auto& win : m_windowManager.getWindows()) {
                if (win.closeRequested) {
                    closeRequests.push_back(win.id);
                }
            }

            for (uint32_t winId : closeRequests) {
                for (auto& win : m_windowManager.getWindowsMutable()) {
                    if (win.id == winId) {
                        win.closeRequested = false;
                        break;
                    }
                }

                uint64_t foundKey = 0;
                SurfaceEntry* foundSurface = nullptr;
                for (auto& [surfKey, entry] : m_surfaces) {
                    if (entry.windowId == winId) {
                        foundKey = surfKey;
                        foundSurface = &entry;
                        break;
                    }
                }

                if (!foundSurface) {
                    m_windowManager.removeWindow(winId);
                    continue;
                }

                const uint32_t surfaceId = static_cast<uint32_t>(foundKey & 0xFFFFFFFFull);
                if (foundSurface->clientFd >= 0) {
                    protocol::LCLHeader destroyHeader{};
                    destroyHeader.opcode = protocol::LCLOpcode::SurfaceDestroy;
                    destroyHeader.payloadSize = sizeof(protocol::LCLMsgSurfaceDestroy);

                    protocol::LCLMsgSurfaceDestroy destroyMsg{};
                    destroyMsg.surfaceId = surfaceId;
                    protocol::sendMsgWithFd(foundSurface->clientFd, destroyHeader, &destroyMsg);
                }

                foundSurface->ignoreBufferCommits = true;
                if (foundSurface->pixels && foundSurface->width > 0 && foundSurface->height > 0) {
                    foundSurface->transitionPhase = SurfaceEntry::TransitionPhase::Closing;
                    foundSurface->transitionElapsedSec = 0.0f;
                    foundSurface->transitionDurationSec = 0.14f;
                    foundSurface->transitionOpacity = 1.0f;
                    foundSurface->transitionScale = 1.0f;
                    foundSurface->pendingDestroy = false;
                } else {
                    m_windowManager.removeWindow(winId);
                    if (foundSurface->pixels && foundSurface->shmSize > 0) {
                        munmap(foundSurface->pixels, foundSurface->shmSize);
                        foundSurface->pixels = nullptr;
                    }
                    if (foundSurface->shmFd >= 0) {
                        close(foundSurface->shmFd);
                        foundSurface->shmFd = -1;
                    }
                    surfacesToRemove.push_back(foundKey);
                }
            }

            for (uint64_t key : surfacesToRemove) {
                m_surfaces.erase(key);
            }
        }

        if (ev.type == InputEventType::KeyboardKey) {
            uint32_t focusedWinId = m_windowManager.getFocusedWindowId();
            if (focusedWinId > 0) {
                for (const auto& [surfKey, entry] : m_surfaces) {
                    if (entry.windowId == focusedWinId && entry.clientFd >= 0) {
                        protocol::LCLHeader header{};
                        header.opcode = protocol::LCLOpcode::InputEvent;
                        header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

                        // 1. Raw Key Event (1 = KeyDown, 2 = KeyUp)
                        protocol::LCLMsgInputEvent inputMsg{};
                        inputMsg.surfaceId = static_cast<uint32_t>(surfKey & 0xFFFFFFFF);
                        inputMsg.type = ev.pressed ? 1 : 2;
                        inputMsg.key = ev.key;
                        inputMsg.pressed = ev.pressed ? 1 : 0;
                        inputMsg.modifiers = ev.modifiers;
                        inputMsg.codepoint = ev.codepoint;

                        protocol::sendMsgWithFd(entry.clientFd, header, &inputMsg);

                        // 2. High-level KeyPress / TextInput Event (type = 5) on keydown when character is printable
                        if (ev.pressed && ev.codepoint != 0) {
                            protocol::LCLMsgInputEvent pressMsg = inputMsg;
                            pressMsg.type = 5; // KeyPress / TextInput
                            protocol::sendMsgWithFd(entry.clientFd, header, &pressMsg);
                        }
                        break;
                    }
                }
            }
        } else if (ev.type == InputEventType::PointerMotion || ev.type == InputEventType::PointerButton) {
            uint32_t focusedWinId = m_windowManager.getFocusedWindowId();
            if (focusedWinId > 0) {
                const render::Window* targetWin = nullptr;
                for (const auto& win : m_windowManager.getWindows()) {
                    if (win.id == focusedWinId) {
                        targetWin = &win;
                        break;
                    }
                }
                if (targetWin) {
                    for (const auto& [surfKey, entry] : m_surfaces) {
                        if (entry.windowId == focusedWinId && entry.clientFd >= 0) {
                            int titleOffset = (targetWin->decorationMode == render::DecorationMode::SSD)
                                              ? DisplayScale::titleBarHeight() : 0;
                            const float bufferScale = sanitizeBufferScale(entry.bufferScale);
                            float localX = static_cast<float>(m_windowManager.getMouseX() - targetWin->x) / bufferScale;
                            float localY = static_cast<float>(m_windowManager.getMouseY() - targetWin->y - titleOffset) / bufferScale;

                            protocol::LCLHeader header{};
                            header.opcode = protocol::LCLOpcode::InputEvent;
                            header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

                            protocol::LCLMsgInputEvent inputMsg{};
                            inputMsg.surfaceId = static_cast<uint32_t>(surfKey & 0xFFFFFFFF);
                            inputMsg.type = (ev.type == InputEventType::PointerMotion) ? 3 : 4;
                            inputMsg.x = localX;
                            inputMsg.y = localY;
                            inputMsg.key = ev.button;
                            inputMsg.pressed = ev.pressed ? 1 : 0;

                            protocol::sendMsgWithFd(entry.clientFd, header, &inputMsg);
                            break;
                        }
                    }
                }
            }
        }
    });

    // --- Launch primary desktop session ---
    m_startupManager.launchDefaultSession(m_windowManager);

    // --- Frame pacing from DRM refresh rate ---
    uint32_t hz = (m_displayManager.isInitialized()
                   ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (hz == 0) hz = 60;
    m_targetFrameDuration = std::chrono::microseconds(1000000 / hz);

    std::cout << "[LCL Core] Pure Display Server active on " << kCompositorSocket
              << ". Listening for client surface registrations.\n"
              << "[LCL Core] High Refresh Rate active: targeting " << hz
              << " Hz (~" << (1000000 / hz) << " us per frame budget).\n";

    m_lastBlinkCheck = std::chrono::steady_clock::now();
    m_lastTransitionTick = m_lastBlinkCheck;
    m_initialized = true;
    return true;
}

// ============================================================
// Per-Tick Processing
// ============================================================

void Compositor::processInput() {
    if (m_inputManager.isInitialized()) {
        m_inputManager.dispatchEvents(
            static_cast<int>(m_renderer.getWidth()),
            static_cast<int>(m_renderer.getHeight()));
    }
}

void Compositor::processIPC() {
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
            m_needsRedraw = true;
            return false;
        }
        return true;
    };

    for (const auto& msg : m_ipcManager.pollMessages()) {
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
                        if (entry.pixels && entry.shmSize > 0) {
                            munmap(entry.pixels, entry.shmSize);
                            entry.pixels = nullptr;
                        }
                        if (entry.shmFd >= 0) {
                            close(entry.shmFd);
                            entry.shmFd = -1;
                        }
                        surfacesToRemove.push_back(surfKey);
                    }
                }
            }
            for (uint64_t key : surfacesToRemove) {
                m_surfaces.erase(key);
            }
            m_needsRedraw = true;
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
                m_needsRedraw = true;
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
            m_needsRedraw = true;

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
                m_needsRedraw = true;
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
                        m_needsRedraw = true;
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
                    if (it->second.pixels && it->second.shmSize > 0) {
                        munmap(it->second.pixels, it->second.shmSize);
                        it->second.pixels = nullptr;
                    }
                    if (it->second.shmFd >= 0) {
                        close(it->second.shmFd);
                        it->second.shmFd = -1;
                    }
                    m_surfaces.erase(it);
                    m_needsRedraw = true;
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
                    m_needsRedraw = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetInsetBorder) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetInsetBorder)) {
                auto* borderMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetInsetBorder*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | borderMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    m_windowManager.setInsetBorderEnabled(it->second.windowId, borderMsg->enabled != 0);
                    m_needsRedraw = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetWindowCornerRadius) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetWindowCornerRadius)) {
                auto* radiusMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetWindowCornerRadius*>(msg.payload.data());
                uint64_t surfaceKey = (static_cast<uint64_t>(msg.pid > 0 ? msg.pid : msg.clientFd) << 32) | radiusMsg->surfaceId;
                auto it = m_surfaces.find(surfaceKey);
                if (it != m_surfaces.end()) {
                    m_windowManager.setWindowCornerRadius(it->second.windowId, radiusMsg->radiusPx * it->second.bufferScale);
                    m_needsRedraw = true;
                }
            }

        } else if (msg.header.opcode == lcl::protocol::LCLOpcode::SetReservedZone) {
            if (msg.payload.size() >= sizeof(lcl::protocol::LCLMsgSetReservedZone)) {
                auto* resMsg = reinterpret_cast<const lcl::protocol::LCLMsgSetReservedZone*>(msg.payload.data());
                m_windowManager.setReservedZone(resMsg->top, resMsg->bottom, resMsg->left, resMsg->right);
                m_needsRedraw = true;
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
                            m_needsRedraw = true;
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
                m_needsRedraw = true;
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
}

void Compositor::publishWindowListToShellClients() {
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

void Compositor::tickCursorBlink() {
    auto now = std::chrono::steady_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastBlinkCheck).count();
    if (ms >= 500) {
        m_lastBlinkCheck = now;
        m_needsRedraw = true;
    }
}

void Compositor::run() {
    if (!m_initialized) return;

    // 1. Query dynamic DRM/KMS monitor refresh rate (default to 60Hz if undetected)
    uint32_t refreshHz = (m_displayManager.isInitialized() ? m_displayManager.getActiveDisplayMode().refreshRate : 60);
    if (refreshHz == 0) refreshHz = 60;

    // 2. Calculate dynamic frame period & headroom allowance (~0.5ms safety budget)
    float targetPeriodMs = 1000.0f / static_cast<float>(refreshHz);
    float headroomMs = std::min(0.5f, targetPeriodMs * 0.08f);
    float targetBudgetMs = targetPeriodMs - headroomMs;
    auto targetFrameDuration = std::chrono::microseconds(static_cast<int64_t>(targetBudgetMs * 1000.0f));

    std::cout << "[LCL Core] Dynamic Frame Pacer Active: " << refreshHz << " Hz "
              << "(Target Period: " << targetPeriodMs << " ms, Headroom: " << headroomMs
              << " ms, Budget: " << targetBudgetMs << " ms)\n";

    m_lastFpsTime = std::chrono::steady_clock::now();

    while (m_running.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        processInput();
        if (m_windowManager.updateAnimations()) {
            m_needsRedraw = true;
        }
        processIPC();
        tickCursorBlink();

        bool willDraw = m_needsRedraw || m_windowManager.isAnyWindowDirty();

        if (willDraw) {
            renderFrame();

            // Dynamic Frame Pacing with Headroom Allowance to prevent frame skipping & uncapped rendering
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - frameStart);
            if (elapsed < targetFrameDuration) {
                std::this_thread::sleep_for(targetFrameDuration - elapsed);
            }
        } else {
            // Idle State: Sleep 2ms to prevent CPU busy-spinning and reduce idle usage to ~0%
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Sliding 1.0s window FPS calculation (displays 0 (Idle) when no frames were rendered)
        auto fpsNow = std::chrono::steady_clock::now();
        float elapsedSec = std::chrono::duration<float>(fpsNow - m_lastFpsTime).count();
        if (elapsedSec >= 1.0f) {
            if (m_fpsFrameCount == 0) {
                m_currentFps = 0.0f;
                m_currentFrameMs = 0.0f;
            } else {
                m_currentFps = static_cast<float>(m_fpsFrameCount) / elapsedSec;
                m_currentFrameMs = (elapsedSec * 1000.0f) / static_cast<float>(m_fpsFrameCount);
            }
            m_fpsFrameCount = 0;
            m_lastFpsTime = fpsNow;
        }

        ++m_loopTicks;
    }
}

void Compositor::renderDiagnosticOverlay() {
    if (!m_showFpsOverlay) return;

    int screenW = static_cast<int>(m_renderer.getWidth());
    int cardW = DisplayScale::px(220);
    int cardH = DisplayScale::px(70);
    int cardX = screenW - cardW - DisplayScale::px(16);
    int cardY = DisplayScale::px(16);

    // Render translucent dark slate card background with sky accent border
    m_renderer.drawFilledRect(cardX, cardY, cardW, cardH, 0xDD0F172A);
    m_renderer.drawRect(cardX, cardY, cardW, cardH, 0x6638BDF8);

    // Determine engine label from audited EGL backend renderer string
    std::string engineStr = "Engine: ";
    auto* egl = m_displayManager.getEGLBackend();
    if (egl && egl->isInitialized()) {
        engineStr += egl->getGLRendererString();
    } else {
        engineStr += "Software Fallback";
    }

    // Determine VSync label
    std::string vsyncStr = "VSync: ";
    if (egl && egl->isInitialized() && egl->isVSyncActive()) {
        vsyncStr += "ON";
    } else {
        vsyncStr += "OFF";
    }

    char fpsBuf[64];
    uint32_t fpsColor = 0xFF4ADE80; // Bright Lime
    if (m_currentFps <= 0.0f) {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: 0 (Idle)");
        fpsColor = 0xFF94A3B8; // Slate Gray when idle
    } else {
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.0f (%.1f ms)", m_currentFps, m_currentFrameMs);
    }

    int textX = cardX + DisplayScale::px(12);
    int textY = cardY + DisplayScale::px(10);
    int lineSpacing = DisplayScale::px(18);

    m_renderer.drawString(textX, textY, fpsBuf, fpsColor);
    m_renderer.drawString(textX, textY + lineSpacing, engineStr, 0xFF38BDF8);     // Cyan Engine
    m_renderer.drawString(textX, textY + lineSpacing * 2, vsyncStr, 0xFF34D399); // Emerald VSync
}

void Compositor::renderFrame() {
    if (!m_needsRedraw && !m_windowManager.isAnyWindowDirty()) return;

    publishWindowListToShellClients();

    const auto now = std::chrono::steady_clock::now();
    float dtSec = std::chrono::duration<float>(now - m_lastTransitionTick).count();
    m_lastTransitionTick = now;
    dtSec = std::clamp(dtSec, 1.0f / 240.0f, 1.0f / 20.0f);

    bool hasActiveTransitions = false;
    for (auto& [_, entry] : m_surfaces) {
        if (entry.transitionPhase == SurfaceEntry::TransitionPhase::None) continue;

        hasActiveTransitions = true;
        entry.transitionElapsedSec += dtSec;
        const float duration = std::max(0.001f, entry.transitionDurationSec);
        const float t = std::clamp(entry.transitionElapsedSec / duration, 0.0f, 1.0f);

        if (entry.transitionPhase == SurfaceEntry::TransitionPhase::Entering) {
            const float y = easeOutCubic(t);
            entry.transitionOpacity = y;
            entry.transitionScale = 0.96f + (0.04f * y);
            if (t >= 1.0f) {
                entry.transitionPhase = SurfaceEntry::TransitionPhase::None;
                entry.transitionOpacity = 1.0f;
                entry.transitionScale = 1.0f;
            }
        } else if (entry.transitionPhase == SurfaceEntry::TransitionPhase::Closing) {
            const float y = easeInCubic(t);
            entry.transitionOpacity = 1.0f - y;
            entry.transitionScale = 1.0f - (0.04f * y);
            if (t >= 1.0f) {
                entry.transitionOpacity = 0.0f;
                entry.transitionScale = 0.96f;
                entry.pendingDestroy = true;
            }
        }
    }

    // --- Build fallback content list for windows without active SHM buffers ---
    std::vector<render::WindowRenderContent> contents;
    for (const auto& win : m_windowManager.getWindows()) {
        bool hasShmBuffer = false;
        for (const auto& [surfKey, entry] : m_surfaces) {
            if (entry.windowId == win.id && entry.pixels) {
                hasShmBuffer = true;
                break;
            }
        }
        if (!hasShmBuffer) {
            render::WindowRenderContent c;
            c.windowId = win.id;
            c.lines = {
                "LCL OS Desktop",
                "Waiting for client surface buffer..."
            };
            c.cursorCol = 0;
            contents.push_back(c);
        }
    }

    // --- Begin Skia frame ---
    auto* skia = m_renderer.getSkiaRenderer();
    skia->beginFrame();

    constexpr float kWindowCornerRadiusPx = 20.0f;
    constexpr float kWindowCornerRoundness = 2.0f;

    auto resolveWindowCornerRadiusPx = [&](const render::Window& win) {
        if (win.cornerRadiusPx >= 0.0f) {
            return win.cornerRadiusPx;
        }

        if (win.decorationMode == render::DecorationMode::SSD) {
            return kWindowCornerRadiusPx;
        }

        if (win.decorationMode == render::DecorationMode::None &&
            win.title.find("Terminal") != std::string::npos) {
            return kWindowCornerRadiusPx;
        }

        return 0.0f;
    };

    auto drawSsdChromeWithLclUi = [&](const render::Window& win,
                                      float chromeOpacity,
                                      float chromeScale,
                                      float scaledTitleHeight) {
        auto fadeUiColor = [&](const lcl::ui::Color& c) {
            return lcl::ui::Color{c.r, c.g, c.b, applyOpacityToAlpha(c.a, chromeOpacity)};
        };

        auto fadeSkiaColor = [&](const lcl::render::SkiaColor& c) {
            return lcl::render::SkiaColor{c.r, c.g, c.b, applyOpacityToAlpha(c.a, chromeOpacity)};
        };

        lcl::ui::RenderPass pass;
        auto root = std::make_unique<lcl::ui::Container>();
        root->setRenderPass(&pass);
        root->setBackgroundColor(lcl::ui::Color{0, 0, 0, 0});
        root->getYogaNode().setWidth(static_cast<float>(win.width));
        root->getYogaNode().setHeight(static_cast<float>(win.height));

        lcl::ui::chrome::HeaderControlsStyle chromeStyle;
        chromeStyle.controlSize = std::max(10.0f, chromeStyle.controlSize * chromeScale);
        chromeStyle.controlGap = std::max(3.0f, chromeStyle.controlGap * chromeScale);
        chromeStyle.minControlLeft = std::max(4.0f, chromeStyle.minControlLeft * chromeScale);
        chromeStyle.minControlTop = std::max(2.0f, chromeStyle.minControlTop * chromeScale);
        chromeStyle.titleMinLeft = std::max(6.0f, static_cast<float>(DisplayScale::px(14)) * chromeScale);
        chromeStyle.titleGapAfterControls = std::max(4.0f, static_cast<float>(DisplayScale::px(12)) * chromeScale);
        chromeStyle.titleRightPadding = std::max(4.0f, static_cast<float>(DisplayScale::px(10)) * chromeScale);
        chromeStyle.glyphFontSize = std::max(8.0f, chromeStyle.glyphFontSize * chromeScale);
        // Titlebar bg is drawn directly below with exact compositor corner geometry.
        chromeStyle.titleBarBackground = lcl::ui::Color{0, 0, 0, 0};
        chromeStyle.titleBarCornerRadiusAdjust = -1.0f;
        chromeStyle.titleBarRoundness = kWindowCornerRoundness;
        chromeStyle.buttonRoundness = 2.0f;
        chromeStyle.controlLeftRadiusOffset = chromeStyle.controlSize * 0.5f;
        chromeStyle.buttonBackground = fadeUiColor(chromeStyle.buttonBackground);
        chromeStyle.buttonBorder = fadeUiColor(chromeStyle.buttonBorder);
        chromeStyle.buttonGlyph = fadeUiColor(chromeStyle.buttonGlyph);
        chromeStyle.titleColor = fadeUiColor(chromeStyle.titleColor);

        const float titleH = std::max(1.0f, scaledTitleHeight);

        const float inset = 1.0f;
        const float bgX = static_cast<float>(win.x) + inset;
        const float bgY = static_cast<float>(win.y) + inset;
        const float bgW = std::max(0.0f, static_cast<float>(win.width) - inset * 2.0f);
        const float bgH = std::max(0.0f, titleH);
        const float bgRadius = std::max(0.0f, (kWindowCornerRadiusPx * chromeScale) - inset);
        const lcl::render::SkiaColor titleBgColor = fadeSkiaColor({17, 19, 23, 255});

        if (bgW > 0.0f && bgH > 0.0f) {
            // Rounded top silhouette aligned with SSD border mask.
            skia->drawRoundedRect(
                {bgX, bgY, bgW, bgH},
                bgRadius,
                titleBgColor,
                {0, 0, 0, 0},
                0.0f,
                kWindowCornerRoundness);

            // Flatten titlebar bottom edge while keeping rounded top corners.
            const float stripH = std::min(bgRadius, bgH);
            if (stripH > 0.0f) {
                skia->drawRect({bgX, bgY + bgH - stripH, bgW, stripH}, titleBgColor);
            }
        }

        auto titleBar = lcl::ui::chrome::buildLibadwaitaTitleBar(
            static_cast<float>(win.width),
            titleH,
            kWindowCornerRadiusPx * chromeScale,
            win.title,
            std::max(9.0f, static_cast<float>(DisplayScale::fontSize()) * chromeScale),
            chromeStyle);

        root->addChild(std::move(titleBar));

        root->getYogaNode().calculateLayout(static_cast<float>(win.width), static_cast<float>(win.height));
        root->syncLayout(static_cast<float>(win.x), static_cast<float>(win.y));

        lcl::ui::Rect damage{static_cast<float>(win.x), static_cast<float>(win.y), static_cast<float>(win.width), static_cast<float>(win.height)};
        root->draw(reinterpret_cast<SkCanvas*>(skia), damage);
    };

    auto drawCsdHeaderControlsOverlay = [&](const render::Window& win) {
        lcl::ui::RenderPass pass;
        auto root = std::make_unique<lcl::ui::Container>();
        root->setRenderPass(&pass);
        root->setBackgroundColor(lcl::ui::Color{0, 0, 0, 0});

        const float titleH = static_cast<float>(DisplayScale::titleBarHeight());
        const float ctrlSize = 16.0f;
        const float ctrlGap = 6.0f;
        const float ctrlLeft = std::max(8.0f, kWindowCornerRadiusPx - 8.0f);
        const float ctrlTop = std::max(4.0f, (titleH - ctrlSize) * 0.5f);

        root->getYogaNode().setWidth(static_cast<float>(win.width));
        root->getYogaNode().setHeight(static_cast<float>(win.height));

        auto mkHeaderControl = [&](float left, const char* glyph) {
            auto button = std::make_unique<lcl::ui::Container>();
            button->setBackgroundColor(lcl::ui::Color{235, 241, 248, 40});
            button->setBorderColor(lcl::ui::Color{230, 238, 248, 92});
            button->setBorderWidth(1.0f);
            button->setBorderRadius(ctrlSize * 0.5f);
            button->setBorderRoundness(2.0f);
            button->getYogaNode().setPositionType(YGPositionTypeAbsolute);
            button->getYogaNode().setPosition(YGEdgeLeft, left);
            button->getYogaNode().setPosition(YGEdgeTop, ctrlTop);
            button->getYogaNode().setWidth(ctrlSize);
            button->getYogaNode().setHeight(ctrlSize);

            auto icon = std::make_unique<lcl::ui::Text>(glyph);
            icon->setTextColor(lcl::ui::Color{232, 240, 248, 210});
            icon->setFontSize(11.0f);
            icon->getYogaNode().setPositionType(YGPositionTypeAbsolute);
            icon->getYogaNode().setPosition(YGEdgeLeft, ctrlSize * 0.32f);
            icon->getYogaNode().setPosition(YGEdgeTop, ctrlSize * 0.16f);
            button->addChild(std::move(icon));

            return button;
        };

        root->addChild(mkHeaderControl(ctrlLeft, "x"));
        root->addChild(mkHeaderControl(ctrlLeft + ctrlSize + ctrlGap, "-"));
        root->addChild(mkHeaderControl(ctrlLeft + (ctrlSize + ctrlGap) * 2.0f, "+"));

        root->getYogaNode().calculateLayout(static_cast<float>(win.width), static_cast<float>(win.height));
        root->syncLayout(static_cast<float>(win.x), static_cast<float>(win.y));

        lcl::ui::Rect damage{static_cast<float>(win.x), static_cast<float>(win.y), static_cast<float>(win.width), static_cast<float>(win.height)};
        root->draw(reinterpret_cast<SkCanvas*>(skia), damage);
    };

    auto drawForcedInsetBorder = [&](const render::Window& win, float opacity, float scale) {
        const uint8_t outerA = applyOpacityToAlpha(120, opacity);
        const uint8_t innerA = applyOpacityToAlpha(86, opacity);
        float baseRadius = resolveWindowCornerRadiusPx(win);
        if (baseRadius <= 0.001f) {
            baseRadius = kWindowCornerRadiusPx;
        }
        const float radius = std::max(0.0f, baseRadius * scale);

        auto* sr = m_renderer.getSkiaRenderer();
        sr->drawRoundedRect(
            {static_cast<float>(win.x), static_cast<float>(win.y), static_cast<float>(win.width), static_cast<float>(win.height)},
            radius,
            {0, 0, 0, 0},
            {10, 12, 16, outerA},
            1.0f,
            kWindowCornerRoundness);

        const float inset = 1.0f;
        const float innerW = std::max(0.0f, static_cast<float>(win.width) - inset * 2.0f);
        const float innerH = std::max(0.0f, static_cast<float>(win.height) - inset * 2.0f);
        if (innerW > 0.0f && innerH > 0.0f) {
            sr->drawRoundedRect(
                {static_cast<float>(win.x) + inset, static_cast<float>(win.y) + inset, innerW, innerH},
                std::max(0.0f, radius - 1.0f),
                {0, 0, 0, 0},
                {245, 248, 252, innerA},
                1.0f,
                kWindowCornerRoundness);
        }
    };

    // 1. Clear Desktop Canvas (Black background)
    m_renderer.clear(0xFF000000);

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    using core::DisplayScale;
    auto applySurfaceRegionEffects = [&](const render::Window& win,
                                         const SurfaceEntry& surface,
                                         protocol::EffectSourceType sourceType,
                                         float windowOpacity) {
        int titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? DisplayScale::titleBarHeight()
            : 0;

        for (const auto& fx : surface.effectRegions) {
            if (fx.region.source != sourceType || fx.filters.empty()) continue;

            int fxX = win.x + (fx.followSurfaceBounds ? 0 : fx.region.x);
            int fxY = win.y + titleOffset + (fx.followSurfaceBounds ? 0 : fx.region.y);
            int fxW = fx.followSurfaceBounds ? static_cast<int>(surface.width) : static_cast<int>(fx.region.width);
            int fxH = fx.followSurfaceBounds ? static_cast<int>(surface.height) : static_cast<int>(fx.region.height);
            if (fxW <= 0 || fxH <= 0) continue;

            // Initial executor supports chain filters with source-type routing.
            // Advanced blend modes are currently treated as normal blend.
            m_renderer.getSkiaRenderer()->applyBackdropFilter(
                fxX, fxY, fxW, fxH,
                std::max(0.0f, fx.region.cornerRadius),
                std::clamp(fx.region.opacity * windowOpacity, 0.0f, 1.0f),
                fx.filters);
        }
    };

    for (const auto& win : m_windowManager.getWindows()) {
        // A. Find matching client SHM surface buffer for this window
        const SurfaceEntry* matchingSurface = nullptr;
        for (const auto& [surfKey, entry] : m_surfaces) {
            if (entry.windowId == win.id && entry.pixels) {
                matchingSurface = &entry;
                break;
            }
        }

        int titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? DisplayScale::titleBarHeight()
            : 0;

        float windowOpacity = 1.0f;
        float windowScale = 1.0f;
        if (matchingSurface) {
            windowOpacity = std::clamp(matchingSurface->transitionOpacity, 0.0f, 1.0f);
            windowScale = std::clamp(matchingSurface->transitionScale, 0.80f, 1.20f);
        }

        const float winCenterX = static_cast<float>(win.x) + static_cast<float>(win.width) * 0.5f;
        const float winCenterY = static_cast<float>(win.y) + static_cast<float>(win.height) * 0.5f;
        const int scaledWinW = std::max(1, static_cast<int>(std::lround(static_cast<float>(win.width) * windowScale)));
        const int scaledWinH = std::max(1, static_cast<int>(std::lround(static_cast<float>(win.height) * windowScale)));
        const int scaledWinX = static_cast<int>(std::lround(winCenterX - static_cast<float>(scaledWinW) * 0.5f));
        const int scaledWinY = static_cast<int>(std::lround(winCenterY - static_cast<float>(scaledWinH) * 0.5f));
        const int scaledTitleOffset = static_cast<int>(std::lround(static_cast<float>(titleOffset) * windowScale));

        // B. Apply effect-graph backdrop regions (new pipeline only)
        if (matchingSurface && !matchingSurface->effectRegions.empty()) {
            applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Backdrop, windowOpacity);
        }

        if (matchingSurface) {
            int dstX = win.x;
            int dstY = win.y + titleOffset;
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            int drawW = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcW) * windowScale)));
            int drawH = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcH) * windowScale)));
            int drawX = static_cast<int>(std::lround(winCenterX + (static_cast<float>(dstX) - winCenterX) * windowScale));
            int drawY = static_cast<int>(std::lround(winCenterY + (static_cast<float>(dstY) - winCenterY) * windowScale));

            if (win.decorationMode == render::DecorationMode::SSD) {
                // Keep a strict non-overlapping partition between SSD titlebar and client content
                // under scaling to avoid double-alpha where layers touch.
                drawX = scaledWinX;
                drawY = scaledWinY + scaledTitleOffset;
                drawW = std::max(1, scaledWinW);
                drawH = std::max(1, scaledWinH - scaledTitleOffset);
            }

            const float windowCornerRadiusPx = resolveWindowCornerRadiusPx(win);
            const bool maskToWindowShape = windowCornerRadiusPx > 0.001f;

            m_renderer.getSkiaRenderer()->drawBuffer(
                drawX, drawY, srcW, srcH,
                reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                stridePixels,
                windowOpacity,
                maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                kWindowCornerRoundness,
                win.decorationMode == render::DecorationMode::SSD,
                drawW,
                drawH);

            if (!matchingSurface->effectRegions.empty()) {
                applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Foreground, windowOpacity);
            }

            // Keep CSD for terminal content while rendering controls through compositor
            // so buttons match SSD quality (AA/blend pipeline) exactly.
            if (win.decorationMode == render::DecorationMode::None &&
                win.title.find("Terminal") != std::string::npos) {
                render::Window scaledOverlayWin = win;
                scaledOverlayWin.x = scaledWinX;
                scaledOverlayWin.y = scaledWinY;
                scaledOverlayWin.width = scaledWinW;
                scaledOverlayWin.height = scaledWinH;
                drawCsdHeaderControlsOverlay(scaledOverlayWin);
            }
        } else {
            // Render text fallback content ONLY for standard SSD decorated application windows
            if (win.decorationMode == render::DecorationMode::SSD) {
                const render::WindowRenderContent* content = nullptr;
                for (const auto& c : contents) {
                    if (c.windowId == win.id) { content = &c; break; }
                }
                if (content) {
                    int titleOffset = DisplayScale::titleBarHeight();
                    int textX = win.x + DisplayScale::windowPad();
                    int textY = win.y + titleOffset + DisplayScale::px(12);
                    for (const auto& line : content->lines) {
                        skia->drawString(textX, textY, line, lcl::theme::UI::TerminalText);
                        textY += DisplayScale::px(18);
                    }
                }
            }
        }

        // C. Render Server-Side Window Frame (Titlebar & Inset Border) on top of content.
        if (win.decorationMode == render::DecorationMode::SSD) {
            render::Window scaledChromeWin = win;
            scaledChromeWin.x = scaledWinX;
            scaledChromeWin.y = scaledWinY;
            scaledChromeWin.width = scaledWinW;
            scaledChromeWin.height = scaledWinH;
            drawSsdChromeWithLclUi(
                scaledChromeWin,
                windowOpacity,
                windowScale,
                static_cast<float>(scaledTitleOffset));
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            render::Window borderWin = win;
            borderWin.x = scaledWinX;
            borderWin.y = scaledWinY;
            borderWin.width = scaledWinW;
            borderWin.height = scaledWinH;
            drawForcedInsetBorder(borderWin, windowOpacity, windowScale);
        }
    }

    // 3. Hardware cursor only (no software cursor fallback)
    if (m_displayManager.isHardwareCursorActive()) {
        m_displayManager.moveHardwareCursor(m_windowManager.getMouseX(), m_windowManager.getMouseY());
    }

    m_renderer.swapBuffers();

    std::vector<uint64_t> surfacesToRemove;
    for (auto& [surfKey, entry] : m_surfaces) {
        if (!entry.pendingDestroy) continue;

        if (entry.windowId > 0) {
            m_windowManager.removeWindow(entry.windowId);
        }
        if (entry.pixels && entry.shmSize > 0) {
            munmap(entry.pixels, entry.shmSize);
            entry.pixels = nullptr;
        }
        if (entry.shmFd >= 0) {
            close(entry.shmFd);
            entry.shmFd = -1;
        }
        surfacesToRemove.push_back(surfKey);
    }
    for (uint64_t key : surfacesToRemove) {
        m_surfaces.erase(key);
    }

    m_windowManager.clearAllDirty();
    m_needsRedraw = hasActiveTransitions || !surfacesToRemove.empty();

    // Increment presented frame count (used by 1.0s sliding window in run loop)
    m_fpsFrameCount++;
}

} // namespace lcl::core
