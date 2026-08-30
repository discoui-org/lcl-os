#pragma once

#include <cstdint>
#include <optional>

#include "system/scene/scene_registry.hpp"

namespace lcl::render {
class WindowManager;
}

namespace lcl::core {

/** Active-scene identity, scoped for the future by seat, display and workspace. */
struct FocusState {
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    SceneId activeSceneId{0};
};

/**
 * Translates WindowManager focus into shell-facing scene focus.  It does not
 * perform hit tests or alter stacking; those remain WindowManager duties.
 */
class FocusController {
public:
    std::optional<FocusState> reconcile(const SceneRegistry& scenes,
                                        const render::WindowManager& windowManager);

    const FocusState& current() const noexcept { return m_current; }

private:
    FocusState m_current{};
};

} // namespace lcl::core
