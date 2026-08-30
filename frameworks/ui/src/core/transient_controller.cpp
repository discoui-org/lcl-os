#include "lcl-ui/core/transient_controller.hpp"

#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/events.hpp"
#include "lcl-ui/widgets/widget.hpp"

#include <algorithm>
#include <utility>

namespace lcl::ui {

TransientController::TransientController(EventDispatcher& dispatcher)
    : m_dispatcher(dispatcher) {}

TransientController::~TransientController() {
    clear();
}

void TransientController::setWindowRoot(Widget* windowRoot) {
    if (m_windowRoot == windowRoot) return;
    clear();
    m_windowRoot = windowRoot;
}

TransientHandle TransientController::registerLocal(std::unique_ptr<Widget> widget,
                                                    TransientOptions options) {
    if (!widget || !m_windowRoot) return 0;

    Widget* local = widget.get();
    m_windowRoot->addChild(std::move(widget));
    const TransientHandle handle = m_nextHandle++;
    m_entries.push_back(Entry{
        handle, Presentation::LocalWidget, local, 0, {}, std::move(options)});
    return handle;
}

TransientHandle TransientController::registerSurface(
        uint32_t surfaceId, std::function<void()> destroySurface,
        TransientOptions options) {
    if (surfaceId == 0 || !destroySurface) return 0;
    const bool alreadyRegistered = std::any_of(
        m_entries.begin(), m_entries.end(), [surfaceId](const Entry& entry) {
            return entry.presentation == Presentation::PopupSurface &&
                entry.surfaceId == surfaceId;
        });
    if (alreadyRegistered) return 0;

    const TransientHandle handle = m_nextHandle++;
    m_entries.push_back(Entry{
        handle, Presentation::PopupSurface, nullptr, surfaceId,
        std::move(destroySurface), std::move(options)});
    return handle;
}

bool TransientController::remove(TransientHandle handle) {
    return removeInternal(handle, false);
}

void TransientController::clear() {
    while (!m_entries.empty()) {
        removeInternal(m_entries.back().handle, false);
    }
}

void TransientController::pruneExpiredOwners() {
    std::vector<TransientHandle> expired;
    for (const auto& entry : m_entries) {
        if (entry.options.trackOwnerLifetime &&
            entry.options.ownerLifetime.expired()) {
            expired.push_back(entry.handle);
        }
    }
    for (const auto handle : expired) removeInternal(handle, false);
}

bool TransientController::contains(TransientHandle handle) const noexcept {
    return std::any_of(m_entries.begin(), m_entries.end(),
        [handle](const Entry& entry) { return entry.handle == handle; });
}

bool TransientController::containsLocalTarget(const Widget* target) const {
    return std::any_of(m_entries.begin(), m_entries.end(), [target](const Entry& entry) {
        return entry.presentation == Presentation::LocalWidget &&
            containsWidget(entry.widget, target);
    });
}

TransientHandle TransientController::outsideDismissCandidate(
        Widget* hitTarget, const PointerEvent& event) const {
    if (m_entries.empty()) return 0;
    if (event.source == PointerSource::Mouse) {
        if (event.type != PointerEventType::Down || event.button != 0) return 0;
    } else if (event.source == PointerSource::Touch) {
        if (event.type != PointerEventType::Up || !event.isTouchTapCompletion()) return 0;
    } else {
        return 0;
    }

    const Entry& topmost = m_entries.back();
    if (!topmost.options.dismissOnOutsidePointer) return 0;
    if (topmost.presentation == Presentation::LocalWidget &&
        containsWidget(topmost.widget, hitTarget)) {
        return 0;
    }
    return topmost.handle;
}

bool TransientController::dismissOutsideCandidate(TransientHandle handle) {
    if (handle == 0) return false;
    return removeInternal(handle, true);
}

bool TransientController::containsWidget(const Widget* ancestor,
                                         const Widget* target) {
    for (const Widget* current = target; current; current = current->getParent()) {
        if (current == ancestor) return true;
    }
    return false;
}

bool TransientController::removeInternal(TransientHandle handle,
                                         bool notifyDismiss) {
    const auto found = std::find_if(m_entries.begin(), m_entries.end(),
        [handle](const Entry& entry) { return entry.handle == handle; });
    if (found == m_entries.end()) return false;

    Entry entry = std::move(*found);
    m_entries.erase(found);

    if (entry.presentation == Presentation::LocalWidget) {
        m_dispatcher.cancelWidgetSubtree(entry.widget);
        if (m_windowRoot) m_windowRoot->removeChild(entry.widget);
    } else if (entry.destroySurface) {
        entry.destroySurface();
    }

    if (notifyDismiss && entry.options.onDismiss) entry.options.onDismiss();
    return true;
}

} // namespace lcl::ui
