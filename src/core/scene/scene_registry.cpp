#include "core/scene/scene_registry.hpp"

#include <algorithm>

#include "render/window_manager.hpp"

namespace lcl::core {

namespace {

const render::Window* findWindow(const render::WindowManager& windowManager, uint32_t windowId) {
    const auto& windows = windowManager.getWindows();
    const auto it = std::find_if(windows.begin(), windows.end(), [windowId](const render::Window& window) {
        return window.id == windowId;
    });
    return it == windows.end() ? nullptr : &*it;
}

} // namespace

SceneId SceneRegistry::mapClientSurface(SurfaceRegistry::Key surfaceKey,
                                        pid_t clientPid,
                                        uint32_t windowId,
                                        std::string appId,
                                        std::string title,
                                        uint64_t appInstanceId,
                                        protocol::LCLDecorationMode decorationMode) {
    const auto existing = m_sceneBySurface.find(surfaceKey);
    if (existing != m_sceneBySurface.end()) {
        return existing->second;
    }

    SceneRecord scene{};
    scene.id = m_nextSceneId++;
    scene.appInstanceId = appInstanceId;
    scene.clientPid = clientPid;
    scene.surfaceKey = surfaceKey;
    scene.windowId = windowId;
    scene.appId = std::move(appId);
    scene.title = std::move(title);
    scene.decorationMode = decorationMode;
    m_sceneBySurface.emplace(surfaceKey, scene.id);
    m_sceneByWindow.emplace(windowId, scene.id);
    m_scenes.push_back(scene);
    queueChange(SceneStateChange::Kind::Added, m_scenes.back());
    return scene.id;
}

void SceneRegistry::markClosing(SurfaceRegistry::Key surfaceKey) {
    const auto idIt = m_sceneBySurface.find(surfaceKey);
    if (idIt == m_sceneBySurface.end()) return;

    auto* scene = findMutable(idIt->second);
    if (scene == nullptr || scene->visibility == SceneVisibility::Closing) return;
    scene->visibility = SceneVisibility::Closing;
    queueChange(SceneStateChange::Kind::Updated, *scene);
}

void SceneRegistry::removeSurface(SurfaceRegistry::Key surfaceKey) {
    const auto idIt = m_sceneBySurface.find(surfaceKey);
    if (idIt == m_sceneBySurface.end()) return;

    const SceneId sceneId = idIt->second;
    const auto sceneIt = std::find_if(m_scenes.begin(), m_scenes.end(), [sceneId](const SceneRecord& scene) {
        return scene.id == sceneId;
    });
    if (sceneIt == m_scenes.end()) {
        m_sceneBySurface.erase(idIt);
        return;
    }

    queueChange(SceneStateChange::Kind::Removed, *sceneIt);
    m_sceneByWindow.erase(sceneIt->windowId);
    m_sceneBySurface.erase(idIt);
    m_scenes.erase(sceneIt);
}

void SceneRegistry::removeWindow(uint32_t windowId) {
    const auto idIt = m_sceneByWindow.find(windowId);
    if (idIt == m_sceneByWindow.end()) return;
    const auto* scene = find(idIt->second);
    if (scene != nullptr) {
        removeSurface(scene->surfaceKey);
    }
}

void SceneRegistry::reconcileWindowState(const render::WindowManager& windowManager) {
    for (auto& scene : m_scenes) {
        const auto* window = findWindow(windowManager, scene.windowId);
        if (window == nullptr || scene.visibility == SceneVisibility::Closing) {
            continue;
        }

        const SceneVisibility visibility = window->isMinimized
            ? SceneVisibility::Minimized
            : SceneVisibility::Visible;
        const auto decorationMode = window->decorationMode ==
                render::DecorationMode::CSD
            ? protocol::LCLDecorationMode::CSD
            : (window->decorationMode == render::DecorationMode::None
                ? protocol::LCLDecorationMode::None
                : protocol::LCLDecorationMode::SSD);
        if (scene.x == window->x && scene.y == window->y &&
            scene.width == window->width && scene.height == window->height &&
            scene.title == window->title && scene.visibility == visibility &&
            scene.decorationMode == decorationMode &&
            scene.edgeToEdge == window->edgeToEdge) {
            continue;
        }

        scene.x = window->x;
        scene.y = window->y;
        scene.width = window->width;
        scene.height = window->height;
        scene.title = window->title;
        scene.visibility = visibility;
        scene.decorationMode = decorationMode;
        scene.edgeToEdge = window->edgeToEdge;
        queueChange(SceneStateChange::Kind::Updated, scene);
    }
}

std::optional<SceneId> SceneRegistry::sceneIdForWindow(uint32_t windowId) const {
    const auto found = m_sceneByWindow.find(windowId);
    if (found == m_sceneByWindow.end()) return std::nullopt;
    return found->second;
}

std::optional<SceneId> SceneRegistry::sceneIdForSurface(SurfaceRegistry::Key surfaceKey) const {
    const auto found = m_sceneBySurface.find(surfaceKey);
    if (found == m_sceneBySurface.end()) return std::nullopt;
    return found->second;
}

const SceneRecord* SceneRegistry::find(SceneId sceneId) const {
    const auto found = std::find_if(m_scenes.begin(), m_scenes.end(), [sceneId](const SceneRecord& scene) {
        return scene.id == sceneId;
    });
    return found == m_scenes.end() ? nullptr : &*found;
}

SceneRecord* SceneRegistry::findMutable(SceneId sceneId) {
    const auto found = std::find_if(m_scenes.begin(), m_scenes.end(), [sceneId](const SceneRecord& scene) {
        return scene.id == sceneId;
    });
    return found == m_scenes.end() ? nullptr : &*found;
}

std::vector<SceneStateChange> SceneRegistry::takePendingChanges() {
    std::vector<SceneStateChange> changes;
    changes.swap(m_pendingChanges);
    return changes;
}

void SceneRegistry::queueChange(SceneStateChange::Kind kind, const SceneRecord& scene) {
    m_pendingChanges.push_back(SceneStateChange{kind, scene});
}

} // namespace lcl::core
