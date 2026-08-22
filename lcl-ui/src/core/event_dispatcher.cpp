#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/touch_interaction.hpp"
#include "lcl-ui/core/transient_controller.hpp"
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lcl::ui {
namespace {

struct FocusTraversalEntry {
    Widget* widget{nullptr};
    bool eligible{false};
};

bool isFocusEligible(const Widget* widget) {
    if (!widget || !widget->isFocusable()) return false;
    for (const Widget* current = widget; current; current = current->getParent()) {
        if (!current->isVisible() || !current->isInteractionEnabled()) return false;
    }
    return true;
}

void collectFocusTraversalEntries(Widget* parent,
                                  std::vector<FocusTraversalEntry>& entries) {
    if (!parent) return;
    for (const auto& child : parent->getChildren()) {
        Widget* widget = child.get();
        entries.push_back(FocusTraversalEntry{widget, isFocusEligible(widget)});
        collectFocusTraversalEntries(widget, entries);
    }
}

Widget* activeFocusScope(Widget* root, Widget* focused) {
    if (!root || !focused) return root;
    bool belongsToRoot = false;
    for (Widget* current = focused; current; current = current->getParent()) {
        if (current == root) {
            belongsToRoot = true;
            break;
        }
    }
    if (!belongsToRoot) return root;

    for (Widget* current = focused->getParent(); current; current = current->getParent()) {
        if (current->isFocusScope()) return current;
        if (current == root) break;
    }
    return root;
}

} // namespace

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

bool KeyEvent::requestFocus(Widget& owner) const {
    if (!m_dispatcher) return false;
    m_dispatcher->setFocus(&owner);
    return m_dispatcher->getFocusedWidget() == &owner;
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
    if (widget && !isFocusEligible(widget)) widget = nullptr;

    Widget* previous = getFocusedWidget();
    if (previous == widget) return;

    m_focusedWidget = nullptr;
    m_focusedLifetime.reset();
    if (previous) {
        FocusEvent lostEv{FocusEventType::Lost};
        previous->onFocusLost(lostEv);
    }

    if (!widget) return;
    m_focusedWidget = widget;
    m_focusedLifetime = widget->getLifetimeToken();
    FocusEvent gainedEv{FocusEventType::Gained};
    widget->onFocusGained(gainedEv);
}

bool EventDispatcher::dispatchPointerEvent(Widget* root, const PointerEvent& event) {
    return dispatchPointerEvent(root, nullptr, event);
}

bool EventDispatcher::dispatchPointerEvent(Widget* root, TransientController* transients,
                                           const PointerEvent& event) {
    if (!root) return false;

    PointerEvent dispatchEvent = event;
    dispatchEvent.m_dispatcher = this;
    TransientHandle outsideDismissCandidate = 0;

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
        if (transients) {
            outsideDismissCandidate =
                transients->outsideDismissCandidate(hitTarget, dispatchEvent);
        }
    } else if (dispatchEvent.type == PointerEventType::Up &&
               dispatchEvent.source == PointerSource::Touch) {
        if (isTouchTapEligible(dispatchEvent.pointerId)) {
            if (Widget* downTarget = getPointerDownTarget(dispatchEvent.pointerId, root)) {
                const Widget* downFocusTarget = findFocusableTarget(downTarget);
                const Widget* upFocusTarget = findFocusableTarget(hitTarget);
                dispatchEvent.m_touchTapCompletion = downFocusTarget == upFocusTarget;
            }
        }
        if (transients) {
            outsideDismissCandidate =
                transients->outsideDismissCandidate(hitTarget, dispatchEvent);
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

    // Dismiss only after the current target and touch-focus path have finished.
    // The snapshotted handle also prevents a callback from dismissing a
    // transient that was opened by this very event.
    if (outsideDismissCandidate != 0 && transients) {
        handled = transients->dismissOutsideCandidate(outsideDismissCandidate) || handled;
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
        if (isFocusEligible(current)) {
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

    // Measure total displacement from PointerDown, never an incremental move
    // delta. Once invalid, the early return above prevents re-validation.
    if (touch_interaction::exceedsSlop(event.x - it->second.downX,
                                       event.y - it->second.downY)) {
        it->second.tapEligible = false;
    }
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

void EventDispatcher::cancelWidgetSubtree(Widget* subtree) {
    if (!subtree) return;

    if (isDescendantOf(getFocusedWidget(), subtree)) setFocus(nullptr);
    if (isDescendantOf(m_hoveredWidget, subtree)) m_hoveredWidget = nullptr;

    std::unordered_set<uint32_t> canceledCapturePointers;
    for (auto it = m_pointerCaptures.begin(); it != m_pointerCaptures.end();) {
        if (!isDescendantOf(it->second.owner, subtree)) {
            ++it;
            continue;
        }
        const uint32_t pointerId = it->first;
        const PointerCapture capture = it->second;
        it = m_pointerCaptures.erase(it);
        canceledCapturePointers.insert(pointerId);
        m_pointerDownTargets.erase(pointerId);
        if (capture.lifetime.expired()) continue;
        PointerEvent cancelEvent{0.0f, 0.0f, 0, 0.0f, 0.0f,
                                 PointerEventType::Cancel, capture.source, pointerId};
        cancelEvent.m_dispatcher = this;
        dispatchToTarget(capture.owner, cancelEvent);
    }

    for (auto it = m_pointerDownTargets.begin(); it != m_pointerDownTargets.end();) {
        if (!isDescendantOf(it->second.target, subtree)) {
            ++it;
            continue;
        }
        const uint32_t pointerId = it->first;
        const PointerDownTarget downTarget = it->second;
        it = m_pointerDownTargets.erase(it);
        if (canceledCapturePointers.contains(pointerId) || downTarget.lifetime.expired()) continue;
        PointerEvent cancelEvent{0.0f, 0.0f, 0, 0.0f, 0.0f,
                                 PointerEventType::Cancel, downTarget.source, pointerId};
        cancelEvent.m_dispatcher = this;
        dispatchPreviewToTarget(downTarget.target, cancelEvent);
        dispatchToTarget(downTarget.target, cancelEvent);
    }
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

bool EventDispatcher::isDescendantOf(const Widget* target, const Widget* ancestor) {
    for (const Widget* current = target; current; current = current->getParent()) {
        if (current == ancestor) return true;
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

Widget* EventDispatcher::validatePointerCapture(Widget* root,
                                                 const PointerEvent& event) {
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

bool EventDispatcher::dispatchKeyEvent(Widget* root, const KeyEvent& event) {
    KeyEvent dispatchEvent = event;
    dispatchEvent.m_dispatcher = this;
    if (root && dispatchEvent.key == lcl::platform::PhysicalKey::Tab) {
        // Consume both phases centrally. Traversal happens only on KeyDown so
        // KeyUp cannot leak to the newly focused widget.
        if (dispatchEvent.type == KeyEventType::KeyUp) return true;
        const bool backwards =
            (dispatchEvent.modifiers & lcl::platform::kModShift) != 0;
        moveFocus(root, backwards);
        return true;
    }

    Widget* focused = getFocusedWidget();
    if (!focused) return false;
    if (!isFocusEligible(focused)) {
        setFocus(nullptr);
        return false;
    }

    Widget* curr = focused;
    bool handled = false;

    while (curr && !handled) {
        if (dispatchEvent.type == KeyEventType::KeyDown) {
            handled = curr->onKeyDown(dispatchEvent);
        } else if (dispatchEvent.type == KeyEventType::KeyUp) {
            handled = curr->onKeyUp(dispatchEvent);
        }
        if (!handled) {
            curr = curr->getParent();
        }
    }
    return handled;
}

bool EventDispatcher::moveFocus(Widget* root, bool backwards) {
    if (!root) return false;

    Widget* focused = getFocusedWidget();
    Widget* scope = activeFocusScope(root, focused);
    std::vector<FocusTraversalEntry> entries;
    collectFocusTraversalEntries(scope, entries);
    if (entries.empty()) {
        setFocus(nullptr);
        return false;
    }

    const auto current = std::find_if(
        entries.begin(), entries.end(), [focused](const auto& entry) {
            return entry.widget == focused;
    });
    if (!focused || current == entries.end()) {
        if (backwards) {
            const auto candidate = std::find_if(
                entries.rbegin(), entries.rend(),
                [](const auto& entry) { return entry.eligible; });
            if (candidate != entries.rend()) {
                setFocus(candidate->widget);
                return true;
            }
        } else {
            const auto candidate = std::find_if(
                entries.begin(), entries.end(),
                [](const auto& entry) { return entry.eligible; });
            if (candidate != entries.end()) {
                setFocus(candidate->widget);
                return true;
            }
        }
        setFocus(nullptr);
        return false;
    }

    const size_t count = entries.size();
    const size_t currentIndex = static_cast<size_t>(current - entries.begin());
    for (size_t step = 1; step <= count; ++step) {
        const size_t index = backwards
            ? (currentIndex + count - (step % count)) % count
            : (currentIndex + step) % count;
        if (!entries[index].eligible) continue;
        setFocus(entries[index].widget);
        return true;
    }

    setFocus(nullptr);
    return false;
}

bool EventDispatcher::dispatchKeyEvent(const KeyEvent& event) {
    return dispatchKeyEvent(nullptr, event);
}

bool EventDispatcher::dispatchTextInputEvent(const TextInputEvent& event) {
    Widget* focused = getFocusedWidget();
    if (!focused) return false;
    if (!isFocusEligible(focused)) {
        setFocus(nullptr);
        return false;
    }

    Widget* curr = focused;
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
