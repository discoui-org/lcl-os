#include "lcl-ui/core/event_dispatcher.hpp"

namespace lcl::ui {

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

    Widget* target = hitTest(root, event.x, event.y);

    // Hover State Management
    if (event.type == PointerEventType::Move) {
        if (m_hoveredWidget != target) {
            if (m_hoveredWidget) {
                PointerEvent leaveEv = event;
                leaveEv.type = PointerEventType::Leave;
                Widget* curr = m_hoveredWidget;
                while (curr && !curr->onPointerLeave(leaveEv)) {
                    curr = curr->getParent();
                }
            }
            m_hoveredWidget = target;
            if (m_hoveredWidget) {
                PointerEvent enterEv = event;
                enterEv.type = PointerEventType::Enter;
                Widget* curr = m_hoveredWidget;
                while (curr && !curr->onPointerEnter(enterEv)) {
                    curr = curr->getParent();
                }
            }
        }
    }

    // Focus Management
    if (event.type == PointerEventType::Down && target && target->isFocusable()) {
        setFocus(target);
    }

    // Dispatch & Event Bubbling to target
    Widget* dispatchTarget = target ? target : m_hoveredWidget;
    Widget* curr = dispatchTarget;
    bool handled = false;

    while (curr && !handled) {
        switch (event.type) {
            case PointerEventType::Move:
                handled = curr->onPointerMove(event);
                break;
            case PointerEventType::Down:
                handled = curr->onPointerDown(event);
                break;
            case PointerEventType::Up:
                handled = curr->onPointerUp(event);
                break;
            case PointerEventType::Scroll:
                handled = curr->onScroll(event);
                break;
            case PointerEventType::Enter:
                handled = curr->onPointerEnter(event);
                break;
            case PointerEventType::Leave:
                handled = curr->onPointerLeave(event);
                break;
        }
        if (!handled) {
            curr = curr->getParent();
        }
    }

    return handled;
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
