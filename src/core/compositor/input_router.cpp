#include "core/compositor/input_router.hpp"

#include "core/compositor/popup_surface_geometry.hpp"
#include "core/compositor/surface_transaction_coordinator.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>
#include <chrono>
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
        event.type == InputEventType::PointerCancel ||
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
        event.type == InputEventType::PointerCancel ||
        event.type == InputEventType::PointerScroll;
    // Mouse coordinates are shared compositor state. Update them once before
    // gesture arbitration or any windowing-policy branch.
    const bool pointerStateChanged =
        event.source == lcl::platform::PointerSource::Mouse &&
        m_windowManager.updatePointerPosition(event);

    // Relative mouse packets do not carry absolute coordinates. Gesture
    // recognition consumes the same compositor-owned cursor position used by
    // hit testing while preserving Mouse as the source sent to clients.
    InputEvent gestureEvent = event;
    if (pointerEvent &&
        event.source == lcl::platform::PointerSource::Mouse) {
        if (!std::isfinite(gestureEvent.absoluteX) ||
            gestureEvent.absoluteX < 0.0) {
            gestureEvent.absoluteX = m_windowManager.getMouseX();
        }
        if (!std::isfinite(gestureEvent.absoluteY) ||
            gestureEvent.absoluteY < 0.0) {
            gestureEvent.absoluteY = m_windowManager.getMouseY();
        }
    }

    bool systemGestureStarted = false;
    auto clearSystemGestureCapture = [&] {
        m_systemGestureCandidateTarget = 0;
        m_activePopupSurface = 0;
        m_activeAttachedSurface = 0;
        if (gestureEvent.source == lcl::platform::PointerSource::Touch) {
            m_touchTargets.erase(gestureEvent.pointerId);
        }
    };
    if (m_systemGesturesEnabled) {
        const bool wasTracking =
            m_systemGestureArena.isTracking(gestureEvent.pointerId);
        SystemGestureProgress progress{};
        const auto gestureTime = gestureEvent.timestampNs != 0
            ? SystemGestureArena::TimePoint(
                std::chrono::nanoseconds(gestureEvent.timestampNs))
            : SystemGestureArena::Clock::now();
        const auto decision = m_systemGestureArena.process(
            gestureEvent, m_windowManager.getScreenHeight(), &progress,
            gestureTime);
        systemGestureStarted =
            decision == SystemGestureDecision::Tracking &&
            gestureEvent.type == InputEventType::PointerButton &&
            gestureEvent.pressed;
        if (decision == SystemGestureDecision::Claim) {
            if (m_systemGestureCandidateTarget != 0) {
                InputEvent cancel = gestureEvent;
                cancel.type = InputEventType::PointerCancel;
                cancel.pressed = false;
                forwardToSurface(cancel, m_systemGestureCandidateTarget);
            }
            clearSystemGestureCapture();
            const bool handled = m_systemGestureHandler
                ? m_systemGestureHandler(decision, progress) : false;
            return pointerStateChanged || handled;
        }
        if (decision == SystemGestureDecision::Update) {
            const bool handled = m_systemGestureHandler
                ? m_systemGestureHandler(decision, progress) : false;
            return pointerStateChanged || handled;
        }
        if (decision == SystemGestureDecision::Cancel) {
            clearSystemGestureCapture();
            const bool handled = m_systemGestureHandler
                ? m_systemGestureHandler(decision, progress) : false;
            return pointerStateChanged || handled;
        }
        if (decision == SystemGestureDecision::Consume) {
            if (gestureEvent.type == InputEventType::PointerCancel) {
                clearSystemGestureCapture();
            }
            return pointerStateChanged;
        }
        if (decision == SystemGestureDecision::Home) {
            clearSystemGestureCapture();
            const bool handled = m_systemGestureHandler
                ? m_systemGestureHandler(SystemGestureDecision::Home, progress)
                : false;
            return pointerStateChanged || handled;
        }
        if (decision == SystemGestureDecision::PassThrough && wasTracking &&
            !m_systemGestureArena.isTracking(gestureEvent.pointerId)) {
            // The arena relinquished an unclaimed stream. Keep normal touch
            // capture alive so the application still receives its release.
            m_systemGestureCandidateTarget = 0;
        }
    }
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
    SurfaceRegistry::Key attachedTarget = 0;
    const auto activeAttached = m_surfaces.find(m_activeAttachedSurface);
    if (pointerEvent && m_activeAttachedSurface != 0 &&
        activeAttached != m_surfaces.end() &&
        !activeAttached->second.pendingDestroy) {
        attachedTarget = m_activeAttachedSurface;
    } else if (pointerEvent && popupTarget == 0) {
        attachedTarget = findAttachedAt(pointerX(), pointerY());
    }

    bool visibilityInputBlocked = false;
    if (event.type == InputEventType::PointerButton && event.pressed) {
        const uint32_t focusedWindowId = m_windowManager.getFocusedWindowId();
        const auto surface = std::find_if(
            m_surfaces.begin(), m_surfaces.end(),
            [focusedWindowId](const auto& item) {
                if (item.second.isAttached() ||
                    item.second.windowId != focusedWindowId) return false;
                const auto phase = item.second.transitionPhase;
                return phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
                       phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing;
            });
        visibilityInputBlocked = surface != m_surfaces.end();
    }
    const bool popupButtonEvent = event.type == InputEventType::PointerButton &&
        popupTarget != 0;
    // WM resize borders remain authoritative even when an interactive Frame
    // attachment covers the same pixels. The Frame owns only the interior
    // press; otherwise a titlebar reaching the window edge makes that edge
    // impossible to resize.
    bool attachedCoversResizeBorder = false;
    if (attachedTarget != 0) {
        const auto attached = m_surfaces.find(attachedTarget);
        if (attached != m_surfaces.end()) {
            attachedCoversResizeBorder =
                m_windowManager.resizeEdgeAt(
                    attached->second.attachedWindowId,
                    pointerX(), pointerY()) != render::ResizeEdge::None;
        }
    }
    const bool attachedButtonEvent = event.type == InputEventType::PointerButton &&
        event.pressed && attachedTarget != 0 && !attachedCoversResizeBorder;
    auto result = (visibilityInputBlocked || popupButtonEvent || attachedButtonEvent ||
                   !m_desktopWindowManagementEnabled)
        ? render::WindowInputResult{}
        : m_windowManager.processWindowManagementEvent(event);
    result.stateChanged = result.stateChanged || pointerStateChanged;
    const bool geometryPressTookPrecedence = attachedTarget != 0 &&
        event.type == InputEventType::PointerButton && event.pressed &&
        result.interaction.isManual();

    if (event.type == InputEventType::PointerMotion) {
        const auto active = m_surfaces.find(m_activePopupSurface);
        popupTarget = m_activePopupSurface != 0 && active != m_surfaces.end() &&
                !active->second.pendingDestroy
            ? m_activePopupSurface
            : findPopupAt(pointerX(), pointerY());
        const auto attached = m_surfaces.find(m_activeAttachedSurface);
        attachedTarget = m_activeAttachedSurface != 0 &&
                attached != m_surfaces.end() &&
                !attached->second.pendingDestroy
            ? m_activeAttachedSurface
            : (popupTarget == 0 ? findAttachedAt(pointerX(), pointerY()) : 0);
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
    } else if (event.type == InputEventType::PointerButton && event.pressed &&
               attachedTarget != 0 && !geometryPressTookPrecedence) {
        const auto attached = m_surfaces.find(attachedTarget);
        if (attached != m_surfaces.end()) {
            m_windowManager.focusWindow(attached->second.attachedWindowId);
            result.stateChanged = true;
        }
        m_activeAttachedSurface = attachedTarget;
        m_surfaces.focusKeyboardSurface(0);
    } else if (event.type == InputEventType::PointerButton && event.pressed) {
        m_surfaces.focusKeyboardSurface(0);
    }
    if (result.interaction) {
        for (auto& [_, entry] : m_surfaces) {
            if (!entry.isAttached() &&
                entry.windowId == result.interaction.windowId) {
                if (result.interaction.isManual()) {
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

    const bool touchPointer = event.source == lcl::platform::PointerSource::Touch &&
        pointerEvent;
    const bool touchDown = touchPointer &&
        event.type == InputEventType::PointerButton && event.pressed;
    const bool touchFinished = touchPointer &&
        ((event.type == InputEventType::PointerButton && !event.pressed) ||
         event.type == InputEventType::PointerCancel);
    if (touchDown || systemGestureStarted) {
        const auto target = popupTarget != 0
            ? popupTarget
            : (attachedTarget != 0 && !geometryPressTookPrecedence
                   ? attachedTarget : focusedSurfaceKey());
        if (touchDown && target != 0) {
            m_touchTargets[event.pointerId] = target;
        }
        if (systemGestureStarted) m_systemGestureCandidateTarget = target;
    }

    const auto capturedTouch = touchPointer
        ? m_touchTargets.find(event.pointerId)
        : m_touchTargets.end();
    const auto keyboardSurfaceKey = m_surfaces.keyboardFocusSurface();
    const auto keyboardSurface = m_surfaces.find(keyboardSurfaceKey);
    if (event.type == InputEventType::KeyboardKey &&
        keyboardSurfaceKey != 0 && keyboardSurface != m_surfaces.end() &&
        !keyboardSurface->second.pendingDestroy) {
        forwardToSurface(event, keyboardSurfaceKey);
    } else if (capturedTouch != m_touchTargets.end()) {
        forwardToSurface(event, capturedTouch->second);
    } else if (popupTarget != 0) {
        forwardToSurface(event, popupTarget);
    } else if (attachedTarget != 0 && !geometryPressTookPrecedence) {
        forwardToSurface(event, attachedTarget);
    } else {
        forwardToFocusedSurface(event);
    }
    if ((event.type == InputEventType::PointerButton && !event.pressed) ||
        event.type == InputEventType::PointerCancel) {
        m_activePopupSurface = 0;
        m_activeAttachedSurface = 0;
    }
    if (touchFinished) m_touchTargets.erase(event.pointerId);
    if (stateChanged) {
        const uint32_t focusedWindowId = m_windowManager.getFocusedWindowId();
        const auto focusedSurface = std::find_if(
            m_surfaces.begin(), m_surfaces.end(),
            [focusedWindowId](const auto& item) {
                return !item.second.isPopup() && !item.second.isAttached() &&
                    item.second.windowId == focusedWindowId;
            });
        if (focusedSurface != m_surfaces.end()) {
            const auto phase = focusedSurface->second.transitionPhase;
            if (phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
                phase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing) {
                // Route the event that initiated the transition to its original
                // target, then expose the underlying window to subsequent input.
                m_windowManager.transferFocusFromWindow(focusedWindowId);
            }
        }
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

        const auto parentSurface = std::find_if(
            m_surfaces.begin(), m_surfaces.end(), [&window](const auto& item) {
                return !item.second.isPopup() && !item.second.isAttached() &&
                    item.second.windowId == window.id;
            });
        const float titleOffset =
            window.decorationMode == render::DecorationMode::SSD ? 32.0f : 0.0f;
        const bool hasPendingGeometryTarget = window.isResizing() ||
            window.isAtomicTargetTransitioning() ||
            window.activeResizeEdge != render::ResizeEdge::None;
        const float configuredX = hasPendingGeometryTarget
            ? window.pendingX : window.x;
        const float configuredY = hasPendingGeometryTarget
            ? window.pendingY : window.y;
        const float logicalContentW = window.pendingWidth > 0.0f
            ? window.pendingWidth : window.width;
        const float logicalContentH = std::max(
            1.0f,
            (window.pendingHeight > 0.0f
                 ? window.pendingHeight : window.height) - titleOffset);
        const bool livePositionChanged =
            hasPendingGeometryTarget && parentSurface != m_surfaces.end() &&
            (configuredX != parentSurface->second.configuredX ||
             configuredY != parentSurface->second.configuredY);
        const bool parentGeometryNeedsConfigure =
            parentSurface != m_surfaces.end() &&
            (logicalContentW != parentSurface->second.configuredWidth ||
             logicalContentH != parentSurface->second.configuredHeight ||
             livePositionChanged);
        const bool resizingGeometry = window.isResizing() ||
            window.isAtomicTargetTransitioning() ||
            window.activeResizeEdge != render::ResizeEdge::None;

        // A pending generation retains the complete old WindowGroup. Keep one
        // raster batch in flight and coalesce every newer pointer target in
        // WindowManager. Replacing an unfinished batch every refresh interval
        // can starve slower parent/frame raster work forever during a drag.
        if (parentSurface != m_surfaces.end() &&
            parentSurface->second.atomicConfigureGeneration != 0) {
            continue;
        }

        std::vector<SurfaceRegistry::Key> atomicAttachments;
        for (const auto attachmentKey : m_surfaces.attachedChildren(window.id)) {
            const auto attachment = m_surfaces.find(attachmentKey);
            if (attachment == m_surfaces.end()) continue;
            const auto& entry = attachment->second;
            if (entry.clientFd < 0 || entry.pendingDestroy ||
                entry.ignoreBufferCommits || !entry.hasCommittedBuffer ||
                !entry.hasRenderableBuffer()) continue;
            const float targetWidth = entry.attachedFollowParentWidth
                ? logicalContentW : entry.attachedWidth;
            const float targetHeight = entry.attachedFollowParentHeight
                ? (window.pendingHeight > 0.0f
                       ? window.pendingHeight : window.height)
                : entry.attachedHeight;
            if (targetWidth != entry.configuredWidth ||
                targetHeight != entry.configuredHeight) {
                atomicAttachments.push_back(attachmentKey);
            }
        }
        const bool atomicResize = resizingGeometry &&
            parentGeometryNeedsConfigure && parentSurface != m_surfaces.end() &&
            parentSurface->second.hasCommittedBuffer &&
            parentSurface->second.hasRenderableBuffer();
        if (atomicResize) {
            const auto blocked = [this, now](
                    const SurfaceRegistry::SurfaceEntry& entry) {
                const bool refreshLimited =
                    entry.lastConfigureSent.time_since_epoch().count() != 0 &&
                    now - entry.lastConfigureSent < m_refreshInterval;
                return SurfaceRegistry::hasOutstandingConfigure(entry) ||
                    SurfaceRegistry::hasUnpresentedFrame(entry) ||
                    refreshLimited;
            };
            bool batchBlocked = blocked(parentSurface->second);
            for (const auto key : atomicAttachments) {
                const auto attachment = m_surfaces.find(key);
                if (attachment == m_surfaces.end() ||
                    blocked(attachment->second)) {
                    batchBlocked = true;
                    break;
                }
            }
            if (batchBlocked) continue;
        }

        bool parentConfigurePublished = !atomicResize;
        for (auto& [surfaceKey, entry] : m_surfaces) {
            if (entry.isAttached() || entry.windowId != window.id ||
                entry.clientFd < 0) {
                continue;
            }

            // AtomicRetained permits one in-flight generation per surface.
            // New pointer targets remain coalesced in WindowManager until the
            // current group has been presented.
            const bool refreshLimited =
                entry.lastConfigureSent.time_since_epoch().count() != 0 &&
                now - entry.lastConfigureSent < m_refreshInterval;
            if (SurfaceRegistry::hasOutstandingConfigure(entry) ||
                SurfaceRegistry::hasUnpresentedFrame(entry) ||
                refreshLimited) {
                break;
            }

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
            configure.geometryGeneration = window.geometryGeneration;
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
                parentConfigurePublished = true;
            }
            break;
        }

        bool atomicAttachmentsPublished = !atomicResize;
        if (atomicResize && parentConfigurePublished) {
            atomicAttachmentsPublished = true;
            for (const auto attachmentKey : atomicAttachments) {
                auto attachment = m_surfaces.find(attachmentKey);
                if (attachment == m_surfaces.end()) {
                    atomicAttachmentsPublished = false;
                    break;
                }
                auto& entry = attachment->second;
                const float width = entry.attachedFollowParentWidth
                    ? logicalContentW : entry.attachedWidth;
                const float height = entry.attachedFollowParentHeight
                    ? (window.pendingHeight > 0.0f
                           ? window.pendingHeight : window.height)
                    : entry.attachedHeight;
                protocol::LCLHeader header{};
                header.opcode = protocol::LCLOpcode::ConfigureBounds;
                header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);
                protocol::LCLMsgConfigureBounds configure{};
                configure.surfaceId = static_cast<uint32_t>(
                    attachmentKey & 0xFFFFFFFFu);
                configure.configureSerial = entry.nextConfigureSerial++;
                configure.geometryGeneration = window.geometryGeneration;
                configure.x = entry.attachedX;
                configure.y = entry.attachedY;
                configure.width = width;
                configure.height = height;
                configure.backingWidth = width;
                configure.backingHeight = height;
                configure.bufferScale = entry.bufferScale;
                configure.resizeReason = (!window.isMaximized &&
                                          window.isResizing())
                    ? protocol::LCLConfigureResizeReason::Interactive
                    : protocol::LCLConfigureResizeReason::WindowStateTransition;
                if (!protocol::sendMsgWithFd(entry.clientFd, header, &configure)) {
                    atomicAttachmentsPublished = false;
                    entry.forceConfigure = true;
                    break;
                }
                entry.pendingConfigureSerial = configure.configureSerial;
                entry.configuredGeometryGeneration = window.geometryGeneration;
                entry.configuredX = entry.attachedX;
                entry.configuredY = entry.attachedY;
                entry.configuredWidth = width;
                entry.configuredHeight = height;
                entry.lastConfigureSent = now;
                entry.forceConfigure = false;
            }
        }

        for (const auto attachmentKey : m_surfaces.attachedChildren(window.id)) {
            auto attachment = m_surfaces.find(attachmentKey);
            if (attachment == m_surfaces.end() ||
                attachment->second.clientFd < 0 ||
                attachment->second.pendingDestroy ||
                attachment->second.ignoreBufferCommits) continue;
            auto& entry = attachment->second;
            // Follow the same newest logical frame target used for the parent
            // surface configure. window.width/height are only the last
            // committed client geometry during interactive resize, so using
            // them here leaves WM chrome stuck at its old extent.
            const float parentWidth = window.pendingWidth > 0.0f
                ? window.pendingWidth : window.width;
            const float parentHeight = window.pendingHeight > 0.0f
                ? window.pendingHeight : window.height;
            const float configuredWidth = entry.attachedFollowParentWidth
                ? parentWidth : entry.attachedWidth;
            const float configuredHeight = entry.attachedFollowParentHeight
                ? parentHeight : entry.attachedHeight;
            if (!entry.forceConfigure &&
                configuredWidth == entry.configuredWidth &&
                configuredHeight == entry.configuredHeight) continue;
            if (resizingGeometry && parentGeometryNeedsConfigure &&
                !parentConfigurePublished) {
                continue;
            }
            const bool atomicAttachment = atomicResize &&
                std::find(atomicAttachments.begin(), atomicAttachments.end(),
                          attachmentKey) != atomicAttachments.end();
            if (atomicAttachment) continue;
            const bool refreshLimited =
                entry.lastConfigureSent.time_since_epoch().count() != 0 &&
                now - entry.lastConfigureSent < m_refreshInterval;
            if (SurfaceRegistry::hasOutstandingConfigure(entry) ||
                SurfaceRegistry::hasUnpresentedFrame(entry) ||
                refreshLimited) continue;

            protocol::LCLHeader header{};
            header.opcode = protocol::LCLOpcode::ConfigureBounds;
            header.payloadSize = sizeof(protocol::LCLMsgConfigureBounds);
            protocol::LCLMsgConfigureBounds configure{};
            configure.surfaceId = static_cast<uint32_t>(
                attachmentKey & 0xFFFFFFFFu);
            configure.configureSerial = entry.nextConfigureSerial++;
            configure.geometryGeneration = window.geometryGeneration;
            configure.x = entry.attachedX;
            configure.y = entry.attachedY;
            configure.width = configuredWidth;
            configure.height = configuredHeight;
            configure.backingWidth = configuredWidth;
            configure.backingHeight = configuredHeight;
            configure.bufferScale = entry.bufferScale;
            configure.resizeReason = (!window.isMaximized &&
                                      (window.isResizing() ||
                                       window.activeResizeEdge !=
                                           render::ResizeEdge::None))
                ? protocol::LCLConfigureResizeReason::Interactive
                : protocol::LCLConfigureResizeReason::WindowStateTransition;
            if (protocol::sendMsgWithFd(entry.clientFd, header, &configure)) {
                entry.pendingConfigureSerial = configure.configureSerial;
                entry.configuredGeometryGeneration = window.geometryGeneration;
                entry.configuredX = entry.attachedX;
                entry.configuredY = entry.attachedY;
                entry.configuredWidth = configuredWidth;
                entry.configuredHeight = configuredHeight;
                entry.lastConfigureSent = now;
                entry.forceConfigure = false;
            }
        }
        if (atomicResize && parentConfigurePublished &&
            atomicAttachmentsPublished) {
            std::vector<SurfaceRegistry::Key> participants;
            participants.reserve(1 + atomicAttachments.size());
            participants.push_back(parentSurface->first);
            participants.insert(participants.end(), atomicAttachments.begin(),
                                atomicAttachments.end());
            // There is no time-based escape into a torn WindowGroup. A slow
            // participant leaves only this group on its retained layer; erase
            // or disconnect cancels the epoch through SurfaceRegistry.
            (void)SurfaceTransactionCoordinator::begin(
                m_surfaces, window.id, window.geometryGeneration,
                participants);
            // begin() refuses to replace an in-flight barrier. This complete
            // configure batch is therefore the group's only raster generation;
            // surface teardown cancels an abandoned epoch.
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
            return !item.second.isPopup() && !item.second.isAttached() &&
                item.second.windowId == windowId;
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
    const auto surfaceKey = focusedSurfaceKey();
    if (surfaceKey != 0) forwardToSurface(event, surfaceKey);
}

SurfaceRegistry::Key InputRouter::focusedSurfaceKey() const {
    const uint32_t focusedWindowId = m_windowManager.getFocusedWindowId();
    if (focusedWindowId == 0) return 0;
    const auto surface = std::find_if(
        m_surfaces.begin(), m_surfaces.end(), [focusedWindowId](const auto& item) {
            return !item.second.isPopup() && !item.second.isAttached() &&
                item.second.windowId == focusedWindowId &&
                item.second.clientFd >= 0;
        });
    return surface == m_surfaces.end() ? 0 : surface->first;
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
    const bool rejectsInput = entry.isAttached()
        ? !entry.attachedAcceptsInput
        : entry.unfocusable;
    if (rejectsInput || !entry.hasCommittedBuffer || entry.pendingDestroy) {
        return;
    }
    if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing ||
        entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing) {
        return;
    }
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::InputEvent;
    header.payloadSize = sizeof(protocol::LCLMsgInputEvent);

    if (event.type == InputEventType::KeyboardKey) {
        protocol::LCLMsgInputEvent input{};
        input.surfaceId = surfaceId;
        input.type = static_cast<uint32_t>(event.pressed
            ? protocol::LCLInputEventType::KeyDown
            : protocol::LCLInputEventType::KeyUp);
        input.key = static_cast<uint32_t>(event.key);
        input.pressed = event.pressed ? 1 : 0;
        input.modifiers = event.modifiers;
        input.codepoint = event.codepoint;
        protocol::sendMsgWithFd(entry.clientFd, header, &input);

        if (event.pressed && event.codepoint != 0) {
            input.type = static_cast<uint32_t>(protocol::LCLInputEventType::TextInput);
            protocol::sendMsgWithFd(entry.clientFd, header, &input);
        }
        return;
    }

    if (event.type != InputEventType::PointerMotion &&
        event.type != InputEventType::PointerButton &&
        event.type != InputEventType::PointerCancel &&
        event.type != InputEventType::PointerScroll) {
        return;
    }

    uint32_t targetWindowId = entry.windowId;
    if (entry.isPopup()) {
        const auto parent = m_surfaces.find(entry.parentSurfaceKey);
        if (parent == m_surfaces.end()) return;
        targetWindowId = parent->second.windowId;
    } else if (entry.isAttached()) {
        targetWindowId = entry.attachedWindowId;
    }
    const auto windowIt = std::find_if(m_windowManager.getWindows().begin(), m_windowManager.getWindows().end(), [targetWindowId](const render::Window& window) {
        return window.id == targetWindowId;
    });
    if (windowIt == m_windowManager.getWindows().end()) {
        return;
    }

    const float titleOffset = !entry.isPopup() && !entry.isAttached() &&
        windowIt->decorationMode == render::DecorationMode::SSD
            ? 32.0f
            : 0.0f;
    protocol::LCLMsgInputEvent input{};
    input.surfaceId = surfaceId;
    if (event.type == InputEventType::PointerMotion) {
        input.type = static_cast<uint32_t>(protocol::LCLInputEventType::PointerMotion);
    } else if (event.type == InputEventType::PointerButton) {
        input.type = static_cast<uint32_t>(protocol::LCLInputEventType::PointerButton);
    } else if (event.type == InputEventType::PointerCancel) {
        input.type = static_cast<uint32_t>(protocol::LCLInputEventType::PointerCancel);
    } else if (event.type == InputEventType::PointerScroll) {
        input.type = static_cast<uint32_t>(protocol::LCLInputEventType::PointerScroll);
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
    } else if (entry.isAttached()) {
        const auto parentSurface = std::find_if(
            m_surfaces.begin(), m_surfaces.end(),
            [targetWindowId](const auto& item) {
                return !item.second.isPopup() && !item.second.isAttached() &&
                    item.second.windowId == targetWindowId;
            });
        const float parentTitleOffset = windowIt->decorationMode ==
                render::DecorationMode::SSD
            ? 32.0f : 0.0f;
        const float parentScale = parentSurface == m_surfaces.end()
            ? 1.0f : parentSurface->second.transitionScale;
        const auto group = parentSurface != m_surfaces.end() &&
                parentSurface->second.launchMorphActive
            ? render::makeWindowGroupTransformToBounds(
                *windowIt, parentTitleOffset,
                {parentSurface->second.launchMorphX,
                 parentSurface->second.launchMorphY,
                 parentSurface->second.launchMorphWidth,
                 parentSurface->second.launchMorphHeight})
            : render::makeWindowGroupTransform(
                *windowIt, parentTitleOffset, parentScale);
        surfacePoint = group.unmapPoint({globalPointerX, globalPointerY});
        surfacePoint.x -= entry.attachedX;
        surfacePoint.y -= entry.attachedY;
    } else {
        const auto group = entry.launchMorphActive
            ? render::makeWindowGroupTransformToBounds(
                *windowIt, titleOffset,
                {entry.launchMorphX, entry.launchMorphY,
                 entry.launchMorphWidth, entry.launchMorphHeight})
            : render::makeWindowGroupTransform(
                *windowIt, titleOffset, entry.transitionScale);
        surfacePoint = group.unmapPoint({globalPointerX, globalPointerY});
        surfacePoint.y -= titleOffset;
    }
    input.x = surfacePoint.x;
    input.y = surfacePoint.y;
    input.key = toClientPointerButton(event.button);
    input.pressed = event.pressed ? 1 : 0;
    input.pointerId = event.pointerId;
    input.source = static_cast<uint8_t>(event.source == lcl::platform::PointerSource::Touch
        ? protocol::LCLPointerSource::Touch
        : protocol::LCLPointerSource::Mouse);
    protocol::sendMsgWithFd(entry.clientFd, header, &input);
}

SurfaceRegistry::Key InputRouter::findAttachedAt(
        float globalX, float globalY) const {
    for (auto window = m_windowManager.getWindows().rbegin();
         window != m_windowManager.getWindows().rend(); ++window) {
        if (window->isMinimized) continue;
        const auto parent = std::find_if(
            m_surfaces.begin(), m_surfaces.end(),
            [&window](const auto& item) {
                return !item.second.isPopup() && !item.second.isAttached() &&
                    item.second.windowId == window->id;
            });
        if (parent == m_surfaces.end()) continue;

        const float titleOffset = window->decorationMode ==
                render::DecorationMode::SSD
            ? 32.0f : 0.0f;
        const auto group = parent->second.launchMorphActive
            ? render::makeWindowGroupTransformToBounds(
                *window, titleOffset,
                {parent->second.launchMorphX,
                 parent->second.launchMorphY,
                 parent->second.launchMorphWidth,
                 parent->second.launchMorphHeight})
            : render::makeWindowGroupTransform(
                *window, titleOffset, parent->second.transitionScale);
        const auto local = group.unmapPoint({globalX, globalY});
        const auto children = m_surfaces.attachedChildren(window->id);
        for (auto childKey = children.rbegin(); childKey != children.rend();
             ++childKey) {
            const auto child = m_surfaces.find(*childKey);
            if (child == m_surfaces.end() ||
                !child->second.attachedAcceptsInput ||
                !child->second.hasCommittedBuffer ||
                !child->second.hasRenderableBuffer() ||
                child->second.pendingDestroy) continue;
            const float width = child->second.attachedFollowParentWidth
                ? group.localBounds.width : child->second.attachedWidth;
            const float height = child->second.attachedFollowParentHeight
                ? group.localBounds.height : child->second.attachedHeight;
            if (local.x >= child->second.attachedX &&
                local.x < child->second.attachedX + width &&
                local.y >= child->second.attachedY &&
                local.y < child->second.attachedY + height) {
                return child->first;
            }
        }
        if (group.containsGlobalPoint(globalX, globalY)) return 0;
    }
    return 0;
}

SurfaceRegistry::Key InputRouter::findPopupAt(float globalX, float globalY) const {
    for (auto window = m_windowManager.getWindows().rbegin();
         window != m_windowManager.getWindows().rend(); ++window) {
        if (window->isMinimized) continue;
        const auto parent = std::find_if(m_surfaces.begin(), m_surfaces.end(),
            [&window](const auto& item) {
                return !item.second.isPopup() && !item.second.isAttached() &&
                    item.second.windowId == window->id;
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
