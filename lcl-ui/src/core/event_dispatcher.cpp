#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/touch_interaction.hpp"
#include <utility>
#include <vector>

namespace lcl::ui {

bool PointerEvent::capturePointer(Widget& owner) const {
    return m_dispatcher && m_dispatcher->capturePointer(pointerId, &owner, source);
}

bool PointerEvent::releasePointerCapture(Widget& owner) const {
    return m_dispatcher && m_dispatcher->releasePointerCapture(pointerId, &owner);
}

bool PointerEvent::hasPointerCapture(const Widget& owner) const {
    return m_dispatcher && m_dispatcher->hasPointerCapture(pointerId, &owner);
}

bool PointerEvent::cancelPointerDownTarget(Widget& newOwner) const {
    return m_dispatcher &&
        m_dispatcher->cancelPointerDownTarget(pointerId, &newOwner, *this);
}

bool PointerEvent::requestFocus(Widget& owner) const {
    if (!m_dispatcher || !owner.isFocusable() || !owner.isVisible() ||
        !owner.isInteractionEnabled()) return false;
    m_dispatcher->setFocus(&owner);
    return true;
}

Widget* EventDispatcher::hitTest(Widget* root, float x, float y) {
    if (!root || !root->isVisible() || !root->containsPresentationPoint(x, y)) {
        return nullptr;
    }

    const auto& children = root->getChildren();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        Widget* hit = hitTest(it->get(), x, y);
        if (hit) {
            return hit;
        }
    }
    return root;
}

void EventDispatcher::setFocus(Widget* widget) {
    if (m_focusedWidget != widget) {
        if (m_focusedWidget) {
            FocusEvent lostEv{FocusEventType::Lost};
            m_focusedWidget->onFocusLost(lostEv);
        }
        m_focusedWidget = widget;
        if (m_focusedWidget) {
            FocusEvent gainedEv{FocusEventType::Gained};
            m_focusedWidget->onFocusGained(gainedEv);
        }
    }
}

bool EventDispatcher::dispatchPointerEvent(Widget* root, const PointerEvent& event) {
    if (!root) return false;

    PointerEvent dispatchEvent = event;
    dispatchEvent.m_dispatcher = this;

    Widget* captured = validatePointerCapture(root, dispatchEvent);
    const bool routeToCapture = captured &&
        (dispatchEvent.type == PointerEventType::Move ||
         dispatchEvent.type == PointerEventType::Up ||
         dispatchEvent.type == PointerEventType::Cancel);
    Widget* hitTarget = hitTest(root, dispatchEvent.x, dispatchEvent.y);
    Widget* target = routeToCapture
        ? captured
        : hitTarget;

    if (dispatchEvent.type == PointerEventType::Down && target) {
        m_pointerDownTargets[dispatchEvent.pointerId] =
            PointerDownTarget{target, target->getLifetimeToken(), dispatchEvent.source,
                              dispatchEvent.x, dispatchEvent.y};
    }

    if (dispatchEvent.type == PointerEventType::Move &&
        dispatchEvent.source == PointerSource::Touch) {
        updateTouchTapEligibility(dispatchEvent.pointerId, dispatchEvent);
    }

    Widget* previewTarget = target;
    if (!captured &&
        (dispatchEvent.type == PointerEventType::Move ||
         dispatchEvent.type == PointerEventType::Up ||
         dispatchEvent.type == PointerEventType::Cancel)) {
        if (Widget* downTarget = getPointerDownTarget(dispatchEvent.pointerId, root)) {
            previewTarget = downTarget;
            if (dispatchEvent.type == PointerEventType::Cancel) target = downTarget;
        }
    }
    if (previewTarget) dispatchPreviewToTarget(previewTarget, dispatchEvent);

    // Preview handlers may claim the pointer after a drag threshold. Re-target
    // this same event immediately so the former child does not receive it.
    if (dispatchEvent.type == PointerEventType::Move ||
        dispatchEvent.type == PointerEventType::Up ||
        dispatchEvent.type == PointerEventType::Cancel) {
        if (Widget* previewCapture = getPointerCapture(dispatchEvent.pointerId)) {
            target = previewCapture;
        }
    }

    // Hover State Management
    if (dispatchEvent.type == PointerEventType::Move) {
        if (m_hoveredWidget != target) {
            if (m_hoveredWidget) {
                PointerEvent leaveEv = dispatchEvent;
                leaveEv.type = PointerEventType::Leave;
                Widget* curr = m_hoveredWidget;
                while (curr && !curr->onPointerLeave(leaveEv)) {
                    curr = curr->getParent();
                }
            }
            m_hoveredWidget = target;
            if (m_hoveredWidget) {
                PointerEvent enterEv = dispatchEvent;
                enterEv.type = PointerEventType::Enter;
                Widget* curr = m_hoveredWidget;
                while (curr && !curr->onPointerEnter(enterEv)) {
                    curr = curr->getParent();
                }
            }
        }
    }

    // Mouse focus changes on down. Touch focus is deferred until its matching
    // PointerUp, so a later gesture owner can cancel the shared down record.
    if (dispatchEvent.type == PointerEventType::Down) {
        applyPointerDownFocus(target, dispatchEvent);
    } else if (dispatchEvent.type == PointerEventType::Up &&
               dispatchEvent.source == PointerSource::Touch) {
        if (isTouchTapEligible(dispatchEvent.pointerId)) {
            if (Widget* downTarget = getPointerDownTarget(dispatchEvent.pointerId, root)) {
                const Widget* downFocusTarget = findFocusableTarget(downTarget);
                const Widget* upFocusTarget = findFocusableTarget(hitTarget);
                dispatchEvent.m_touchTapCompletion = downFocusTarget == upFocusTarget;
            }
        }
    }

    // Dispatch & Event Bubbling to target
    Widget* dispatchTarget = target ? target : m_hoveredWidget;
    Widget* curr = dispatchTarget;
    bool handled = false;

    while (curr && !handled) {
        switch (dispatchEvent.type) {
            case PointerEventType::Move:
                handled = curr->onPointerMove(dispatchEvent);
                break;
            case PointerEventType::Down:
                handled = curr->onPointerDown(dispatchEvent);
                break;
            case PointerEventType::Up:
                handled = curr->onPointerUp(dispatchEvent);
                break;
            case PointerEventType::Cancel:
                handled = curr->onPointerCancel(dispatchEvent);
                break;
            case PointerEventType::Scroll:
                handled = curr->onScroll(dispatchEvent);
                break;
            case PointerEventType::Enter:
                handled = curr->onPointerEnter(dispatchEvent);
                break;
            case PointerEventType::Leave:
                handled = curr->onPointerLeave(dispatchEvent);
                break;
        }
        if (!handled) {
            curr = curr->getParent();
        }
    }

    if (dispatchEvent.type == PointerEventType::Up &&
        dispatchEvent.source == PointerSource::Touch &&
        dispatchEvent.m_touchTapCompletion &&
        isTouchTapEligible(dispatchEvent.pointerId)) {
        applyTouchTapFocus(getPointerDownTarget(dispatchEvent.pointerId, root),
                           hitTarget, dispatchEvent);
    }

    // Touch input lift (PointerUp) terminates any active hover state
    if ((dispatchEvent.type == PointerEventType::Up ||
         dispatchEvent.type == PointerEventType::Cancel) &&
        dispatchEvent.source == PointerSource::Touch && m_hoveredWidget) {
        PointerEvent leaveEv = dispatchEvent;
        leaveEv.type = PointerEventType::Leave;
        Widget* hoverCurr = m_hoveredWidget;
        while (hoverCurr && !hoverCurr->onPointerLeave(leaveEv)) {
            hoverCurr = hoverCurr->getParent();
        }
        m_hoveredWidget = nullptr;
    }

    if (dispatchEvent.type == PointerEventType::Up ||
        dispatchEvent.type == PointerEventType::Cancel) {
        clearPointerCapture(dispatchEvent.pointerId);
        m_pointerDownTargets.erase(dispatchEvent.pointerId);
    }

    return handled;
}

Widget* EventDispatcher::findFocusableTarget(Widget* target) {
    for (Widget* current = target; current; current = current->getParent()) {
        if (current->isFocusable() && current->isVisible() &&
            current->isInteractionEnabled()) {
            return current;
        }
    }
    return nullptr;
}

void EventDispatcher::updateTouchTapEligibility(uint32_t pointerId,
                                                const PointerEvent& event) {
    const auto it = m_pointerDownTargets.find(pointerId);
    if (it == m_pointerDownTargets.end() ||
        it->second.source != PointerSource::Touch || !it->second.tapEligible) {
        return;
    }

    it->second.tapEligible = !touch_interaction::exceedsSlop(
        event.x - it->second.downX, event.y - it->second.downY);
}

void EventDispatcher::invalidateTouchTapForCapture(uint32_t pointerId) {
    const auto it = m_pointerDownTargets.find(pointerId);
    if (it == m_pointerDownTargets.end() ||
        it->second.source != PointerSource::Touch) {
        return;
    }

    // Capture changes routing/ownership for this interaction. A focus-changing
    // tap remains valid only for an uncaptured down-target sequence.
    it->second.tapEligible = false;
}

bool EventDispatcher::isTouchTapEligible(uint32_t pointerId) const {
    const auto it = m_pointerDownTargets.find(pointerId);
    return it != m_pointerDownTargets.end() &&
        it->second.source == PointerSource::Touch &&
        it->second.tapEligible && !it->second.lifetime.expired();
}

void EventDispatcher::applyPointerDownFocus(Widget* target, const PointerEvent& event) {
    if (event.source != PointerSource::Mouse || event.button != 0) return;

    Widget* focusTarget = findFocusableTarget(target);
    if (!focusTarget) {
        setFocus(nullptr);
    } else if (focusTarget->shouldFocusOnPointerDown(event)) {
        setFocus(focusTarget);
    }
}

void EventDispatcher::applyTouchTapFocus(Widget* downTarget, Widget* upTarget,
                                         const PointerEvent& event) {
    if (!downTarget || findFocusableTarget(downTarget) != findFocusableTarget(upTarget)) {
        return;
    }

    Widget* focusTarget = findFocusableTarget(downTarget);
    if (!focusTarget || focusTarget->shouldFocusOnTouchTap(event)) {
        setFocus(focusTarget);
    }
}

bool EventDispatcher::capturePointer(uint32_t pointerId, Widget* owner,
                                     PointerSource source) {
    if (!owner || !owner->isVisible() || !owner->isInteractionEnabled()) return false;
    invalidateTouchTapForCapture(pointerId);
    m_pointerCaptures[pointerId] =
        PointerCapture{owner, owner->getLifetimeToken(), source};
    return true;
}

bool EventDispatcher::releasePointerCapture(uint32_t pointerId, const Widget* owner) {
    const auto it = m_pointerCaptures.find(pointerId);
    if (it == m_pointerCaptures.end() || (owner && it->second.owner != owner)) return false;
    m_pointerCaptures.erase(it);
    return true;
}

bool EventDispatcher::hasPointerCapture(uint32_t pointerId, const Widget* owner) const {
    const auto it = m_pointerCaptures.find(pointerId);
    return it != m_pointerCaptures.end() && !it->second.lifetime.expired() &&
        (!owner || it->second.owner == owner);
}

Widget* EventDispatcher::getPointerCapture(uint32_t pointerId) const {
    const auto it = m_pointerCaptures.find(pointerId);
    if (it == m_pointerCaptures.end() || it->second.lifetime.expired()) return nullptr;
    return it->second.owner;
}

bool EventDispatcher::cancelPointerDownTarget(uint32_t pointerId, Widget* newOwner,
                                              const PointerEvent& sourceEvent) {
    const auto it = m_pointerDownTargets.find(pointerId);
    if (it == m_pointerDownTargets.end()) return false;
    const PointerDownTarget downTarget = it->second;
    m_pointerDownTargets.erase(it);
    if (downTarget.lifetime.expired() || downTarget.target == newOwner) return false;

    PointerEvent cancelEvent = sourceEvent;
    cancelEvent.type = PointerEventType::Cancel;
    cancelEvent.m_dispatcher = this;
    return dispatchCancelUntil(downTarget.target, newOwner, cancelEvent);
}

void EventDispatcher::cancelPointerCaptures() {
    auto captures = std::move(m_pointerCaptures);
    auto downTargets = std::move(m_pointerDownTargets);
    m_pointerCaptures.clear();
    m_pointerDownTargets.clear();
    for (const auto& [pointerId, capture] : captures) {
        downTargets.erase(pointerId);
        if (capture.lifetime.expired()) continue;
        PointerEvent cancelEvent{0.0f, 0.0f, 0, 0.0f, 0.0f,
                                 PointerEventType::Cancel, capture.source, pointerId};
        cancelEvent.m_dispatcher = this;
        dispatchToTarget(capture.owner, cancelEvent);
    }
    for (const auto& [pointerId, downTarget] : downTargets) {
        if (downTarget.lifetime.expired()) continue;
        PointerEvent cancelEvent{0.0f, 0.0f, 0, 0.0f, 0.0f,
                                 PointerEventType::Cancel,
                                 downTarget.source, pointerId};
        cancelEvent.m_dispatcher = this;
        dispatchPreviewToTarget(downTarget.target, cancelEvent);
        dispatchToTarget(downTarget.target, cancelEvent);
    }
    // Cancellation is terminal; callbacks cannot establish a replacement
    // capture while the owning tree/input lifecycle is being torn down.
    m_pointerCaptures.clear();
    m_pointerDownTargets.clear();
}

bool EventDispatcher::isEventCapableInTree(Widget* root, const Widget* target,
                                           bool ancestorsVisible) {
    if (!root) return false;
    const bool visible = ancestorsVisible && root->isVisible();
    if (root == target) return visible && root->isInteractionEnabled();
    if (!visible) return false;
    for (const auto& child : root->getChildren()) {
        if (isEventCapableInTree(child.get(), target, visible)) return true;
    }
    return false;
}

bool EventDispatcher::dispatchToTarget(Widget* target, const PointerEvent& event) {
    bool handled = false;
    for (Widget* curr = target; curr && !handled; curr = curr->getParent()) {
        handled = curr->onPointerCancel(event);
    }
    return handled;
}

void EventDispatcher::dispatchPreviewToTarget(Widget* target,
                                               const PointerEvent& event) {
    std::vector<Widget*> path;
    for (Widget* curr = target; curr; curr = curr->getParent()) {
        path.push_back(curr);
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        (*it)->onPointerEventPreview(event);
    }
}

bool EventDispatcher::dispatchCancelUntil(Widget* target, Widget* stopBefore,
                                          const PointerEvent& event) {
    bool handled = false;
    for (Widget* curr = target; curr && curr != stopBefore && !handled;
         curr = curr->getParent()) {
        handled = curr->onPointerCancel(event);
    }
    return handled;
}

Widget* EventDispatcher::getPointerDownTarget(uint32_t pointerId, Widget* root) {
    const auto it = m_pointerDownTargets.find(pointerId);
    if (it == m_pointerDownTargets.end()) return nullptr;
    if (it->second.lifetime.expired() ||
        !isEventCapableInTree(root, it->second.target)) {
        m_pointerDownTargets.erase(it);
        return nullptr;
    }
    return it->second.target;
}

Widget* EventDispatcher::validatePointerCapture(Widget* root, const PointerEvent& event) {
    const auto it = m_pointerCaptures.find(event.pointerId);
    if (it == m_pointerCaptures.end()) return nullptr;
    if (it->second.lifetime.expired()) {
        m_pointerCaptures.erase(it);
        return nullptr;
    }
    Widget* owner = it->second.owner;
    if (isEventCapableInTree(root, owner)) return owner;

    m_pointerCaptures.erase(it);
    PointerEvent cancelEvent = event;
    cancelEvent.type = PointerEventType::Cancel;
    cancelEvent.m_dispatcher = this;
    dispatchToTarget(owner, cancelEvent);
    return nullptr;
}

void EventDispatcher::clearPointerCapture(uint32_t pointerId) {
    m_pointerCaptures.erase(pointerId);
}

bool EventDispatcher::dispatchKeyEvent(const KeyEvent& event) {
    if (!m_focusedWidget) return false;

    Widget* curr = m_focusedWidget;
    bool handled = false;

    while (curr && !handled) {
        if (event.type == KeyEventType::KeyDown) {
            handled = curr->onKeyDown(event);
        } else if (event.type == KeyEventType::KeyUp) {
            handled = curr->onKeyUp(event);
        }
        if (!handled) {
            curr = curr->getParent();
        }
    }
    return handled;
}

bool EventDispatcher::dispatchTextInputEvent(const TextInputEvent& event) {
    if (!m_focusedWidget) return false;

    Widget* curr = m_focusedWidget;
    bool handled = false;

    while (curr && !handled) {
        handled = curr->onTextInput(event);
        if (!handled) {
            curr = curr->getParent();
        }
    }
    return handled;
}

} // namespace lcl::ui
