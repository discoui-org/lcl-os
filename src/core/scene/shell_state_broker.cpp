#include "core/scene/shell_state_broker.hpp"

#include <algorithm>

namespace lcl::core {

void ShellStateBroker::publish(const SceneStateChange& change) {
    ShellStateDelta delta{};
    switch (change.kind) {
        case SceneStateChange::Kind::Added:
            delta.kind = ShellStateDelta::Kind::SceneAdded;
            break;
        case SceneStateChange::Kind::Updated:
            delta.kind = ShellStateDelta::Kind::SceneUpdated;
            break;
        case SceneStateChange::Kind::Removed:
            delta.kind = ShellStateDelta::Kind::SceneRemoved;
            break;
    }
    delta.scene = change.scene;
    append(std::move(delta));
}

void ShellStateBroker::publish(const FocusState& focus) {
    ShellStateDelta delta{};
    delta.kind = ShellStateDelta::Kind::FocusChanged;
    delta.focus = focus;
    append(std::move(delta));
}

ShellStateSnapshot ShellStateBroker::snapshot(const SceneRegistry& scenes,
                                              const FocusController& focus) const {
    return ShellStateSnapshot{m_revision, scenes.snapshot(), focus.current()};
}

ShellStateDeltaBatch ShellStateBroker::deltasSince(uint64_t revision) const {
    ShellStateDeltaBatch batch{};
    batch.revision = m_revision;
    if (revision == m_revision) return batch;

    if (revision > m_revision || (!m_deltas.empty() && revision + 1 < m_deltas.front().revision)) {
        batch.requiresSnapshot = true;
        return batch;
    }

    const auto first = std::find_if(m_deltas.begin(), m_deltas.end(), [revision](const ShellStateDelta& delta) {
        return delta.revision > revision;
    });
    batch.deltas.assign(first, m_deltas.end());
    return batch;
}

void ShellStateBroker::append(ShellStateDelta delta) {
    delta.revision = ++m_revision;
    m_deltas.push_back(std::move(delta));
    if (m_deltas.size() > kMaxRetainedDeltas) {
        m_deltas.erase(m_deltas.begin(), m_deltas.begin() +
            static_cast<std::ptrdiff_t>(m_deltas.size() - kMaxRetainedDeltas));
    }
}

} // namespace lcl::core
