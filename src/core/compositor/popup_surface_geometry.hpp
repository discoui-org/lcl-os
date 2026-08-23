#pragma once

#include "core/compositor/surface_registry.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>

namespace lcl::core {

struct PopupSurfaceBounds {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    graphics::Matrix3 globalToLocal{};

    graphics::PointF unmapPoint(float globalX, float globalY) const noexcept {
        return globalToLocal.mapPoint({globalX, globalY});
    }
};

/** Resolve a popup as part of its parent's presented WindowGroup. */
inline PopupSurfaceBounds resolvePopupSurfaceBounds(
        const render::Window& parentWindow,
        const SurfaceRegistry::SurfaceEntry& parentSurface,
        const SurfaceRegistry::SurfaceEntry& popup) noexcept {
    const auto group = render::makeWindowGroupTransform(
        parentWindow, 0.0f, parentSurface.transitionScale);
    const auto popupToGlobal = graphics::Matrix3::translation(
        popup.popupX, popup.popupY).followedBy(group.localToGlobal);
    const auto bounds = popupToGlobal.mapRect(
        {0.0f, 0.0f, popup.initialWidth, popup.initialHeight});
    return {bounds.x, bounds.y, bounds.width, bounds.height,
            popupToGlobal.inverted().value_or(graphics::Matrix3::identity())};
}

} // namespace lcl::core
