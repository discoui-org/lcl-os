#include "lcl-ui/core/event_dispatcher.hpp"
#include <utility>

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
    Widget* target = routeToCapture
        ? captured
        : hitTest(root, dispatchEvent.x, dispatchEvent.y);

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

    // Focus Management
    if (dispatchEvent.type == PointerEventType::Down && target && target->isFocusable()) {
        setFocus(target);
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
    }

    return handled;
}

bool EventDispatcher::capturePointer(uint32_t pointerId, Widget* owner,
                                     PointerSource source) {
    if (!owner || !owner->isVisible() || !owner->isInteractionEnabled()) return false;
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

void EventDispatcher::cancelPointerCaptures() {
    auto captures = std::move(m_pointerCaptures);
    m_pointerCaptures.clear();
    for (const auto& [pointerId, capture] : captures) {
        if (capture.lifetime.expired()) continue;
        PointerEvent cancelEvent{0.0f, 0.0f, 0, 0.0f, 0.0f,
                                 PointerEventType::Cancel, capture.source, pointerId};
        cancelEvent.m_dispatcher = this;
        dispatchToTarget(capture.owner, cancelEvent);
    }
    // Cancellation is terminal; callbacks cannot establish a replacement
    // capture while the owning tree/input lifecycle is being torn down.
    m_pointerCaptures.clear();
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
