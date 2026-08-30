#include "system/shells/state/shell_state_model.hpp"

#include <algorithm>

namespace lcl::shell {

bool ShellStateModel::applySnapshot(const ShellStateSnapshot& snapshot) {
    m_snapshot = snapshot;
    m_hasSnapshot = true;
    return true;
}

bool ShellStateModel::applyDelta(const ShellStateDelta& delta) {
    if (!m_hasSnapshot || delta.revision != m_snapshot.revision + 1) return false;

    m_snapshot.revision = delta.revision;
    m_snapshot.seatId = delta.seatId;
    m_snapshot.displayId = delta.displayId;
    m_snapshot.workspaceId = delta.workspaceId;
    m_snapshot.activeSceneId = delta.activeSceneId;
    if (delta.kind == protocol::LCLShellStateDeltaKind::FocusChanged) return true;

    const auto found = std::find_if(m_snapshot.scenes.begin(), m_snapshot.scenes.end(),
        [&](const ShellScene& scene) { return scene.sceneId == delta.scene.sceneId; });
    if (delta.kind == protocol::LCLShellStateDeltaKind::SceneRemoved) {
        if (found != m_snapshot.scenes.end()) m_snapshot.scenes.erase(found);
        return true;
    }
    if (found == m_snapshot.scenes.end()) {
        m_snapshot.scenes.push_back(delta.scene);
    } else {
        *found = delta.scene;
    }
    return true;
}

std::vector<ShellScene> ShellStateModel::recents() const {
    std::vector<ShellScene> result;
    if (!m_hasSnapshot) return result;
    result.reserve(m_snapshot.scenes.size());
    for (const auto& scene : m_snapshot.scenes) {
        if (scene.visibility != protocol::LCLSceneVisibility::Closing) result.push_back(scene);
    }
    return result;
}

} // namespace lcl::shell
