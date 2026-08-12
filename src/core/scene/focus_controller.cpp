#include "core/scene/focus_controller.hpp"

#include "render/window_manager.hpp"

namespace lcl::core {

std::optional<FocusState> FocusController::reconcile(const SceneRegistry& scenes,
                                                      const render::WindowManager& windowManager) {
    FocusState next = m_current;
    next.activeSceneId = 0;
    if (const auto sceneId = scenes.sceneIdForWindow(windowManager.getFocusedWindowId())) {
        next.activeSceneId = *sceneId;
        if (const auto* scene = scenes.find(*sceneId)) {
            next.displayId = scene->displayId;
            next.workspaceId = scene->workspaceId;
        }
    }

    if (next.seatId == m_current.seatId && next.displayId == m_current.displayId &&
        next.workspaceId == m_current.workspaceId && next.activeSceneId == m_current.activeSceneId) {
        return std::nullopt;
    }

    m_current = next;
    return m_current;
}

} // namespace lcl::core
