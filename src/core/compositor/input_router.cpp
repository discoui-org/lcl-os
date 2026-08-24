#include "core/compositor/input_router.hpp"

#include "core/compositor/popup_surface_geometry.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lcl::core {

namespace {
uint32_t toClientPointerButton(lcl::platform::PointerButton button) {
    if (button == lcl::platform::PointerButton::Left) return 0; // Primary / Left
    if (button == lcl::platform::PointerButton::Middle) return 1; // Middle
    if (button == lcl::platform::PointerButton::Right) return 2; // Secondary / Right
    return 0;
}

} // namespace

bool InputRouter::route(const InputEvent& physicalEvent) {
    InputEvent event = physicalEvent;
    const float outputScale = m_outputScale;
    if (event.type == InputEventType::PointerMotion ||
        event.type == InputEventType::PointerButton ||
        event.type == InputEventType::PointerScroll) {
        if (std::isfinite(event.absoluteX) && event.absoluteX >= 0.0) {
            event.absoluteX /= outputScale;
        }
        if (std::isfinite(event.absoluteY) && event.absoluteY >= 0.0) {
            event.absoluteY /= outputScale;
        }
        event.dx /= outputScale;
        event.dy /= outputScale;
    }
    const bool pointerEvent = event.type == InputEventType::PointerMotion ||
        event.type == InputEventType::PointerButton ||
        event.type == InputEventType::PointerScroll;
    auto pointerX = [&] {
        return std::isfinite(event.absoluteX) && event.absoluteX >= 0.0
            ? static_cast<float>(event.absoluteX)
            : static_cast<float>(m_windowManager.getMouseX());
    };
    auto pointerY = [&] {
        return std::isfinite(event.absoluteY) && event.absoluteY >= 0.0
            ? static_cast<float>(event.absoluteY)
            : static_cast<float>(m_windowManager.getMouseY());
    };

    SurfaceRegistry::Key popupTarget = 0;
    const auto activePopup = m_surfaces.find(m_activePopupSurface);
    if (pointerEvent && m_activePopupSurface != 0 &&
        activePopup != m_surfaces.end() && !activePopup->second.pendingDestroy) {
        popupTarget = m_activePopupSurface;
    } else if (pointerEvent) {
        popupTarget = findPopupAt(pointerX(), pointerY());
    }

    bool visibilityInputBlocked = false;
    if (event.type == InputEventType::PointerButton && event.pressed) {
        const uint32_t focusedWindowId = m_windowManager.getFocusedWindowId();
        const auto surface = std::find_if(
            m_surfaces.begin(), m_surfaces.end(),
            [focusedWindowId](const auto& item) {
                if (item.second.windowId != focusedWindowId) return false;
                const auto phase = item.second.transitionPhase;
                return phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
                       phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring ||
                       phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing;
            });
        visibilityInputBlocked = surface != m_surfaces.end();
    }
    const bool popupButtonEvent = event.type == InputEventType::PointerButton &&
        popupTarget != 0;
    auto result = (visibilityInputBlocked || popupButtonEvent)
        ? render::WindowInputResult{}
        : m_windowManager.processInputEvent(event);

    if (event.type == InputEventType::PointerMotion) {
        const auto active = m_surfaces.find(m_activePopupSurface);
        popupTarget = m_activePopupSurface != 0 && active != m_surfaces.end() &&
                !active->second.pendingDestroy
            ? m_activePopupSurface
            : findPopupAt(pointerX(), pointerY());
    }

    if (event.type == InputEventType::PointerButton && event.pressed &&
        popupTarget != 0) {
        const auto popup = m_surfaces.find(popupTarget);
        const auto parent = popup == m_surfaces.end()
            ? m_surfaces.end()
            : m_surfaces.find(popup->second.parentSurfaceKey);
        if (parent != m_surfaces.end() && parent->second.windowId != 0) {
            m_windowManager.focusWindow(parent->second.windowId);
            result.stateChanged = true;
        }
        m_activePopupSurface = popupTarget;
        m_surfaces.focusKeyboardSurface(popupTarget);
    } else if (event.type == InputEventType::PointerButton && event.pressed) {
        m_surfaces.focusKeyboardSurface(0);
    }
    if (result.interaction) {
        for (auto& [_, entry] : m_surfaces) {
            if (entry.windowId == result.interaction.windowId) {
                if (result.interaction.isWindowStateTransition() &&
                    entry.resizePresentation ==
                        protocol::LCLResizePresentationMode::CompositorMorph) {
                    const auto& previous = result.interaction.previousBounds;
                    SurfaceRegistry::beginGeometryTransition(
                        entry, result.interaction.generation,
                        previous.x, previous.y, previous.width, previous.height,
                        result.interaction.previousWasMaximized,
                        result.interaction.previousWasMinimized);
                } else if (result.interaction.isManual()) {
                    SurfaceRegistry::interruptGeometryTransaction(
                        entry, result.interaction.generation);
                }
            }
        }
    }
    const bool stateChanged = result.stateChanged;
    if (stateChanged) {
        sendPendingConfigures();
        processCloseRequests();
    }

    const auto keyboardSurfaceKey = m_surfaces.keyboardFocusSurface();
    const auto keyboardSurface = m_surfaces.find(keyboardSurfaceKey);
    if (event.type == InputEventType::KeyboardKey &&
        keyboardSurfaceKey != 0 && keyboardSurface != m_surfaces.end() &&
        !keyboardSurface->second.pendingDestroy) {
        forwardToSurface(event, keyboardSurfaceKey);
    } else if (popupTarget != 0) {
        forwardToSurface(event, popupTarget);
    } else {
        forwardToFocusedSurface(event);
    }
    if (event.type == InputEventType::PointerButton && !event.pressed) {
        m_activePopupSurface = 0;
    }
    return stateChanged;
}

void InputRouter::syncWindowState() {
    sendPendingConfigures();
}

void InputRouter::sendPendingConfigures() {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& window : m_windowManager.getWindows()) {
        if (window.isMinimized) {
            continue;
        }
        for (auto& [surfaceKey, entry] : m_surfaces) {
            if (entry.windowId != window.id || entry.clientFd < 0) {
                continue;
            }

            // Every resize policy permits only one configure at a time. Live
            // additionally waits until the accepted client buffer has actually been
            // presented before publishing the newest coalesced geometry.
            const bool live = entry.resizePresentation ==
                protocol::LCLResizePresentationMode::Live;
            const bool refreshLimited = live &&
                entry.lastConfigureSent.time_since_epoch().count() != 0 &&
                now - entry.lastConfigureSent < m_refreshInterval;
            if (SurfaceRegistry::hasOutstandingConfigure(entry) ||
                (live && SurfaceRegistry::hasUnpresentedFrame(entry)) ||
                refreshLimited) {
                break;
            }

            const float titleOffset = (window.decorationMode == render::DecorationMode::SSD)
                ? 32.0f
                : 0.0f;
            const float configuredX = window.isLiveTransitioning() ? window.pendingX : window.x;
            const float configuredY = window.isLiveTransitioning() ? window.pendingY : window.y;
            const float logicalContentW = window.pendingWidth > 0.0f
                ? window.pendingWidth : window.width;
            const float logicalContentH = std::max(1.0f,
                (window.pendingHeight > 0.0f ? window.pendingHeight : window.height) - titleOffset);
            const bool livePositionChanged = window.isLiveTransitioning() &&
                (configuredX != entry.configuredX || configuredY != entry.configuredY);
            if (logicalContentW == entry.configuredWidth && logicalContentH == entry.configuredHeight &&
                !entry.forceConfigure &&
                !livePositionChanged) {
                break;
            }
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);

            protocol::LCLMsgConfigureBounds configure{};
            configure.surfaceId = static_cast<uint32_t>(surfaceKey & 0xFFFFFFFFu);
            configure.configureSerial = entry.nextConfigureSerial++;
            configure.x = configuredX;
            configure.y = configuredY;
            configure.width = logicalContentW;
            configure.height = logicalContentH;
            configure.bufferScale = entry.bufferScale;
            configure.resizeReason = (!window.isMaximized &&
                                      (window.isResizing() || window.activeResizeEdge != render::ResizeEdge::None))
                ? protocol::LCLConfigureResizeReason::Interactive
                : protocol::LCLConfigureResizeReason::WindowStateTransition;
            configure.backingWidth = configure.width;
            configure.backingHeight = configure.height;
            if (live &&
                (configure.resizeReason == protocol::LCLConfigureResizeReason::Interactive ||
                 configure.resizeReason == protocol::LCLConfigureResizeReason::WindowStateTransition)) {
                configure.backingWidth = m_windowManager.getScreenWidth();
                configure.backingHeight = m_windowManager.getScreenHeight();
            }
            configure.isFocused = window.isFocused ? 1 : 0;
            if (protocol::sendMsgWithFd(entry.clientFd, header, &configure)) {
                entry.pendingConfigureSerial = configure.configureSerial;
                entry.configuredGeometryGeneration = window.geometryGeneration;
                entry.configuredX = configuredX;
                entry.configuredY = configuredY;
                entry.configuredWidth = logicalContentW;
                entry.configuredHeight = logicalContentH;
                entry.configuredFocused = configure.isFocused;
                entry.lastConfigureSent = now;
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
            return !item.second.isPopup() && item.second.windowId == windowId;
        });
        if (surfaceIt == m_surfaces.end()) {
            m_windowManager.removeWindow(windowId);
            m_scenes.removeWindow(windowId);
            continue;
        }

        const auto surfaceId = static_cast<uint32_t>(surfaceIt->first & 0xFFFFFFFFu);
        destroyPopupChildren(surfaceIt->first);
        m_surfaces.releaseKeyboardFocus(surfaceIt->first);
        auto& entry = surfaceIt->second;
        if (entry.clientFd >= 0) {
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::SurfaceDestroy;
            header.payloadSize = sizeof(protocol::LCLMsgSurfaceDestroy);
            protocol::LCLMsgSurfaceDestroy destroy{};
            destroy.surfaceId = surfaceId;
            protocol::sendMsgWithFd(entry.clientFd, header, &destroy);
        }

        if (!SurfaceRegistry::beginClosingTransition(entry)) {
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

    const auto surfaceIt = std::find_if(m_surfaces.begin(), m_surfaces.end(), [focusedWindowId](const auto& item) {
        return !item.second.isPopup() && item.second.windowId == focusedWindowId &&
            item.second.clientFd >= 0;
    });
    if (surfaceIt == m_surfaces.end()) {
        return;
    }

    forwardToSurface(event, surfaceIt->first);
}

void InputRouter::forwardToSurface(const InputEvent& event,
                                   SurfaceRegistry::Key surfaceKey) const {
    const auto surfaceIt = m_surfaces.find(surfaceKey);
    if (surfaceIt == m_surfaces.end()) return;

    const auto surfaceId = static_cast<uint32_t>(surfaceIt->first & 0xFFFFFFFFu);
    const auto& entry = surfaceIt->second;
    // System panels and surfaces that have not committed a complete frame are
    // never normal client input targets, even if focus state was stale. Geometry
    // morphs deliberately remain interactive while their presentation catches up.
    if (entry.unfocusable || !entry.hasCommittedBuffer || entry.pendingDestroy) {
        return;
    }
    if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
        entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring ||
        entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing) {
        return;
    }
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::InputEvent;
    header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

    if (event.type == InputEventType::KeyboardKey) {
        protocol::LCLMsgInputEvent input{};
        input.surfaceId = surfaceId;
        input.type = event.pressed ? 1 : 2;
        input.key = static_cast<uint32_t>(event.key);
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

    if (event.type != InputEventType::PointerMotion &&
        event.type != InputEventType::PointerButton &&
        event.type != InputEventType::PointerScroll) {
        return;
    }

    uint32_t targetWindowId = entry.windowId;
    if (entry.isPopup()) {
        const auto parent = m_surfaces.find(entry.parentSurfaceKey);
        if (parent == m_surfaces.end()) return;
        targetWindowId = parent->second.windowId;
    }
    const auto windowIt = std::find_if(m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(), [targetWindowId](const render::Window& window) {
        return window.id == targetWindowId;
    });
    if (windowIt == m_windowManager.getWindows().end()) {
        return;
    }

    const float titleOffset = !entry.isPopup() &&
        windowIt->decorationMode == render::DecorationMode::SSD
            ? 32.0f
            : 0.0f;
    protocol::LCLMsgInputEvent input{};
    input.surfaceId = surfaceId;
    if (event.type == InputEventType::PointerMotion) {
        input.type = 3;
    } else if (event.type == InputEventType::PointerButton) {
        input.type = 4;
    } else if (event.type == InputEventType::PointerScroll) {
        input.type = 6;
        input.deltaX = static_cast<float>(event.dx);
        input.deltaY = static_cast<float>(event.dy);
    }
    // Touch release clears WindowManager's hover cursor before this client
    // forwarding step. Prefer the immutable event coordinates when the input
    // backend supplied them so CSD receives the actual release position.
    const float globalPointerX =
        std::isfinite(event.absoluteX) && event.absoluteX >= 0.0
            ? static_cast<float>(event.absoluteX)
            : static_cast<float>(m_windowManager.getMouseX());
    const float globalPointerY =
        std::isfinite(event.absoluteY) && event.absoluteY >= 0.0
            ? static_cast<float>(event.absoluteY)
            : static_cast<float>(m_windowManager.getMouseY());
    graphics::PointF surfacePoint{};
    if (entry.isPopup()) {
        const auto parent = m_surfaces.find(entry.parentSurfaceKey);
        if (parent == m_surfaces.end()) return;
        const auto popupBounds = resolvePopupSurfaceBounds(*windowIt, parent->second, entry);
        surfacePoint = popupBounds.unmapPoint(globalPointerX, globalPointerY);
    } else {
        const auto group = render::makeWindowGroupTransform(
            *windowIt, titleOffset, entry.transitionScale);
        surfacePoint = group.unmapPoint({globalPointerX, globalPointerY});
        surfacePoint.y -= titleOffset;
    }
    input.x = surfacePoint.x;
    input.y = surfacePoint.y;
    input.key = toClientPointerButton(event.button);
    input.pressed = event.pressed ? 1 : 0;
    input.source = static_cast<uint8_t>(event.source == lcl::platform::PointerSource::Touch
        ? protocol::LCLPointerSource::Touch
        : protocol::LCLPointerSource::Mouse);
    protocol::sendMsgWithFd(entry.clientFd, header, &input);
}

SurfaceRegistry::Key InputRouter::findPopupAt(float globalX, float globalY) const {
    for (auto window = m_windowManager.getWindows().rbegin();
         window != m_windowManager.getWindows().rend(); ++window) {
        if (window->isMinimized) continue;
        const auto parent = std::find_if(m_surfaces.begin(), m_surfaces.end(),
            [&window](const auto& item) {
                return !item.second.isPopup() && item.second.windowId == window->id;
            });
        if (parent != m_surfaces.end()) {
            const auto children = m_surfaces.popupChildren(parent->first);
            for (auto childKey = children.rbegin(); childKey != children.rend(); ++childKey) {
                const auto child = m_surfaces.find(*childKey);
                if (child == m_surfaces.end() || !child->second.hasCommittedBuffer ||
                    !child->second.hasRenderableBuffer() || child->second.pendingDestroy) {
                    continue;
                }
                const auto popupBounds = resolvePopupSurfaceBounds(
                    *window, parent->second, child->second);
                if (globalX >= popupBounds.x &&
                    globalX < popupBounds.x + popupBounds.width &&
                    globalY >= popupBounds.y &&
                    globalY < popupBounds.y + popupBounds.height) {
                    return child->first;
                }
            }
        }

        // A higher unrelated WindowGroup occludes popups belonging to groups
        // below it, exactly matching compositor render order.
        const float scale = parent == m_surfaces.end()
            ? 1.0f : parent->second.transitionScale;
        const auto group = render::makeWindowGroupTransform(*window, 0.0f, scale);
        if (group.containsGlobalPoint(globalX, globalY)) {
            return 0;
        }
    }
    return 0;
}

void InputRouter::destroyPopupChildren(SurfaceRegistry::Key parentSurfaceKey) {
    for (const auto childKey : m_surfaces.popupChildren(parentSurfaceKey)) {
        auto child = m_surfaces.find(childKey);
        if (child == m_surfaces.end()) continue;
        if (child->second.clientFd >= 0) {
            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::SurfaceDestroy;
            header.payloadSize = sizeof(protocol::LCLMsgSurfaceDestroy);
            protocol::LCLMsgSurfaceDestroy destroy{};
            destroy.surfaceId = static_cast<uint32_t>(childKey & 0xFFFFFFFFu);
            protocol::sendMsgWithFd(child->second.clientFd, header, &destroy);
        }
        child->second.ignoreBufferCommits = true;
        child->second.pendingDestroy = true;
        if (m_activePopupSurface == childKey) m_activePopupSurface = 0;
        m_surfaces.releaseKeyboardFocus(childKey);
    }
}

} // namespace lcl::core
