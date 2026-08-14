#include "core/compositor/input_router.hpp"
#include "lcl-motion/motion.hpp"

#include "core/display/display_scale.hpp"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <vector>

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

uint32_t toClientPointerButton(uint32_t linuxButton) {
    switch (linuxButton) {
        case BTN_LEFT: return 0;
        case BTN_MIDDLE: return 1;
        case BTN_RIGHT: return 2;
        default: return linuxButton;
    }
}

} // namespace

bool InputRouter::route(const InputEvent& event) {
    const bool stateChanged = m_windowManager.processInputEvent(event);
    if (stateChanged) {
        sendPendingConfigures();
        processCloseRequests();
    }

    forwardToFocusedSurface(event);
    return stateChanged;
}

void InputRouter::syncWindowState() {
    sendPendingConfigures();
}

void InputRouter::sendPendingConfigures() {
    for (const auto& window : m_windowManager.getWindows()) {
        if (window.isMinimized) {
            continue;
        }
        for (auto& [surfaceKey, entry] : m_surfaces) {
            if (entry.windowId != window.id || entry.clientFd < 0) {
                continue;
            }

            // ConfigureBounds is a one-in-flight transaction. Pointer motion
            // may continue updating Window::pending* while the client renders,
            // but issuing another serial here would make the expected reply
            // stale before it can reach the compositor. Once that commit is
            // accepted, processIPC() calls syncWindowState() again and sends
            // the newest coalesced geometry.
            if (SurfaceRegistry::hasOutstandingConfigure(entry)) {
                break;
            }

            const int titleOffset = (window.decorationMode == render::DecorationMode::SSD)
                ? DisplayScale::titleBarHeight()
                : 0;
            const int configuredX = window.liveResizeTransitionActive ? window.pendingX : window.x;
            const int configuredY = window.liveResizeTransitionActive ? window.pendingY : window.y;
            const uint32_t physicalContentW = static_cast<uint32_t>(window.pendingWidth > 0 ? window.pendingWidth : window.width);
            const uint32_t physicalContentH = static_cast<uint32_t>(std::max(1, (window.pendingHeight > 0 ? window.pendingHeight : window.height) - titleOffset));
            const bool livePositionChanged = window.liveResizeTransitionActive &&
                (configuredX != entry.configuredX || configuredY != entry.configuredY);
            if (physicalContentW == entry.width && physicalContentH == entry.height &&
                entry.pendingConfigureSerial == entry.acceptedConfigureSerial && !entry.forceConfigure &&
                !livePositionChanged) {
                break;
            }
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

            protocol::LCLMsgConfigureBounds configure{};
            configure.surfaceId = static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu);
            configure.configureSerial = entry.nextConfigureSerial++;
            configure.x = logicalToPhysical(configuredX, 1.0f / entry.bufferScale);
            configure.y = logicalToPhysical(configuredY, 1.0f / entry.bufferScale);
            configure.width = physicalToLogical(physicalContentW, entry.bufferScale);
            configure.height = physicalToLogical(physicalContentH, entry.bufferScale);
            configure.bufferScale = entry.bufferScale;
            configure.resizeReason = (!window.isMaximized &&
                                      (window.isResizing || window.activeResizeEdge != render::ResizeEdge::None))
                ? protocol::LCLConfigureResizeReason::Interactive
                : protocol::LCLConfigureResizeReason::WindowStateTransition;
            configure.isFocused = window.isFocused ? 1 : 0;
            if (protocol::sendMsgWithFd(entry.clientFd, header, &configure)) {
                entry.pendingConfigureSerial = configure.configureSerial;
                entry.configuredX = configuredX;
                entry.configuredY = configuredY;
                entry.configuredWidth = physicalContentW;
                entry.configuredHeight = physicalContentH;
                entry.configuredFocused = configure.isFocused;
                entry.lastConfigureSent = std::chrono::steady_clock::now();
                entry.forceConfigure = false;
            }
            break;
        }
    }
}

void InputRouter::processCloseRequests() {
    std::vector<uint32_t> closeRequests;
    for (const auto& window : m_windowManager.getWindows()) {
        if (window.closeRequested) {
            closeRequests.push_back(window.id);
        }
    }

    for (const uint32_t windowId : closeRequests) {
        for (auto& window : m_windowManager.getWindowsMutable()) {
            if (window.id == windowId) {
                window.closeRequested = false;
                break;
            }
        }

        auto surfaceIt = std::find_if(m_surfaces.begin(), m_surfaces.end(), [windowId](const auto& item) {
            return item.second.windowId == windowId;
        });
        if (surfaceIt == m_surfaces.end()) {
            m_windowManager.removeWindow(windowId);
            m_scenes.removeWindow(windowId);
            continue;
        }

        const auto surfaceId = static_cast<uint32_t>(surfaceIt->first & 0xFFFFFFFFu);
        auto& entry = surfaceIt->second;
        if (entry.clientFd >= 0) {
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::SurfaceDestroy;
            header.payloadSize = sizeof(protocol::LCLMsgSurfaceDestroy);
            protocol::LCLMsgSurfaceDestroy destroy{};
            destroy.surfaceId = surfaceId;
            protocol::sendMsgWithFd(entry.clientFd, header, &destroy);
        }

        if (!startClosingTransition(entry)) {
            m_windowManager.removeWindow(windowId);
            m_scenes.removeSurface(surfaceIt->first);
            m_surfaces.erase(surfaceIt);
        } else {
            m_scenes.markClosing(surfaceIt->first);
        }
    }
}

void InputRouter::forwardToFocusedSurface(const InputEvent& event) const {
    const uint32_t focusedWindowId = m_windowManager.getFocusedWindowId();
    if (focusedWindowId == 0) {
        return;
    }

    auto surfaceIt = std::find_if(m_surfaces.begin(), m_surfaces.end(), [focusedWindowId](const auto& item) {
        return item.second.windowId == focusedWindowId && item.second.clientFd >= 0;
    });
    if (surfaceIt == m_surfaces.end()) {
        return;
    }

    const auto surfaceId = static_cast<uint32_t>(surfaceIt->first & 0xFFFFFFFFu);
    const auto& entry = surfaceIt->second;
    // System panels and surfaces that have not committed a complete frame are
    // never normal client input targets, even if focus state was stale. Geometry
    // morphs deliberately remain interactive while their presentation catches up.
    if (entry.unfocusable || !entry.hasCommittedBuffer) {
        return;
    }
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::InputEvent;
    header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

    if (event.type == InputEventType::KeyboardKey) {
        protocol::LCLMsgInputEvent input{};
        input.surfaceId = surfaceId;
        input.type = event.pressed ? 1 : 2;
        input.key = event.key;
        input.pressed = event.pressed ? 1 : 0;
        input.modifiers = event.modifiers;
        input.codepoint = event.codepoint;
        protocol::sendMsgWithFd(entry.clientFd, header, &input);

        if (event.pressed && event.codepoint != 0) {
            input.type = 5;
            protocol::sendMsgWithFd(entry.clientFd, header, &input);
        }
        return;
    }

    if (event.type != InputEventType::PointerMotion && event.type != InputEventType::PointerButton) {
        return;
    }

    const auto windowIt = std::find_if(m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(), [focusedWindowId](const render::Window& window) {
        return window.id == focusedWindowId;
    });
    if (windowIt == m_windowManager.getWindows().end()) {
        return;
    }

    const int titleOffset = (windowIt->decorationMode == render::DecorationMode::SSD)
        ? DisplayScale::titleBarHeight()
        : 0;
    const float scale = sanitizeBufferScale(entry.bufferScale);
    protocol::LCLMsgInputEvent input{};
    input.surfaceId = surfaceId;
    input.type = event.type == InputEventType::PointerMotion ? 3 : 4;
    input.x = static_cast<float>(m_windowManager.getMouseX() - windowIt->x) / scale;
    input.y = static_cast<float>(m_windowManager.getMouseY() - windowIt->y - titleOffset) / scale;
    // lcl-ui's backend-independent pointer contract uses 0 for primary.
    // The compositor continues to use raw BTN_* codes for its own shortcuts.
    input.key = toClientPointerButton(event.button);
    input.pressed = event.pressed ? 1 : 0;
    protocol::sendMsgWithFd(entry.clientFd, header, &input);
}

bool InputRouter::startClosingTransition(SurfaceRegistry::SurfaceEntry& entry) noexcept {
    entry.ignoreBufferCommits = true;
    if (!entry.pixels || entry.width == 0 || entry.height == 0) {
        return false;
    }

    entry.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing;
    entry.transitionElapsedSec = 0.0f;
    entry.transitionDurationSec = lcl::motion::tokens::windowClose().tweenParams.durationSec;
    entry.transitionOpacity = 1.0f;
    entry.transitionScale = 1.0f;
    entry.pendingDestroy = false;
    return true;
}

} // namespace lcl::core
