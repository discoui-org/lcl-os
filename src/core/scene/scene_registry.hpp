#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

#include "core/compositor/surface_registry.hpp"

namespace lcl::render {
class WindowManager;
}

namespace lcl::core {

/** A stable compositor-owned identity for a client-visible scene/window. */
using SceneId = uint64_t;

enum class SceneVisibility : uint8_t {
    Visible,
    Minimized,
    Closing,
};

/**
 * Shell-facing state for one client surface.  This deliberately contains no
 * renderer resources and no widget state, so desktop and future mobile shells
 * can consume exactly the same authority.
 */
struct SceneRecord {
    SceneId id{0};
    uint64_t appInstanceId{0}; // Stable sessiond process-instance identity.
    pid_t clientPid{0};
    SurfaceRegistry::Key surfaceKey{0};
    uint32_t windowId{0};
    std::string appId;
    std::string title;
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    SceneVisibility visibility{SceneVisibility::Visible};
};

struct SceneStateChange {
    enum class Kind : uint8_t {
        Added,
        Updated,
        Removed,
    };

    Kind kind{Kind::Updated};
    SceneRecord scene{};
};

/**
 * Maps mapped client surfaces to shell-visible scenes.  It owns identity and
 * lifecycle metadata only; WindowManager remains the geometry/focus executor
 * and SurfaceRegistry remains the SHM/FD owner.
 */
class SceneRegistry {
public:
    SceneId mapClientSurface(SurfaceRegistry::Key surfaceKey,
                             pid_t clientPid,
                             uint32_t windowId,
                             std::string appId,
                             std::string title,
                             uint64_t appInstanceId = 0);

    void markClosing(SurfaceRegistry::Key surfaceKey);
    void removeSurface(SurfaceRegistry::Key surfaceKey);
    void removeWindow(uint32_t windowId);

    /** Reflect WindowManager-owned geometry and minimize state into scenes. */
    void reconcileWindowState(const render::WindowManager& windowManager);

    std::optional<SceneId> sceneIdForWindow(uint32_t windowId) const;
    std::optional<SceneId> sceneIdForSurface(SurfaceRegistry::Key surfaceKey) const;
    const SceneRecord* find(SceneId sceneId) const;
    const std::vector<SceneRecord>& snapshot() const noexcept { return m_scenes; }

    /** Consume lifecycle changes produced since the compositor last flushed. */
    std::vector<SceneStateChange> takePendingChanges();

private:
    void queueChange(SceneStateChange::Kind kind, const SceneRecord& scene);
    SceneRecord* findMutable(SceneId sceneId);

    SceneId m_nextSceneId{1};
    std::vector<SceneRecord> m_scenes;
    std::unordered_map<SurfaceRegistry::Key, SceneId> m_sceneBySurface;
    std::unordered_map<uint32_t, SceneId> m_sceneByWindow;
    std::vector<SceneStateChange> m_pendingChanges;
};

} // namespace lcl::core
