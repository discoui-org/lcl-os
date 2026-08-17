#pragma once

#include "core/compositor/surface_registry.hpp"
#include "render/window_manager.hpp"

#include <algorithm>

namespace lcl::core {

struct PopupSurfaceBounds {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

/** Resolve a popup as part of its parent's presented WindowGroup. */
inline PopupSurfaceBounds resolvePopupSurfaceBounds(
        const render::Window& parentWindow,
        const SurfaceRegistry::SurfaceEntry& parentSurface,
        const SurfaceRegistry::SurfaceEntry& popup) noexcept {
    const auto parentBounds = render::presentedBounds(parentWindow);
    const float scale = std::clamp(parentSurface.transitionScale, 0.80f, 1.20f);
    const float groupX = parentBounds.x + parentBounds.width * (1.0f - scale) * 0.5f;
    const float groupY = parentBounds.y + parentBounds.height * (1.0f - scale) * 0.5f;
    return {
        groupX + static_cast<float>(popup.popupX) * scale,
        groupY + static_cast<float>(popup.popupY) * scale,
        static_cast<float>(popup.width) * scale,
        static_cast<float>(popup.height) * scale,
    };
}

} // namespace lcl::core
