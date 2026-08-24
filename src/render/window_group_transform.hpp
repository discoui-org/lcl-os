#pragma once

#include <algorithm>
#include <cmath>

#include "lcl-graphics/geometry.hpp"
#include "render/window_manager.hpp"

namespace lcl::render {

/**
 * The single presentation transform shared by every visual and interactive
 * part of a window group.
 *
 * Local coordinates are the logical coordinates of the currently presented
 * window frame. The group scale is applied around that frame's center. Device
 * scale is deliberately not represented here; it belongs to the raster edge.
 */
struct WindowGroupTransform {
    graphics::RectF localBounds{};
    graphics::RectF globalBounds{};
    graphics::Matrix3 localToGlobal{};
    graphics::Matrix3 globalToLocal{};
    float titleHeight{0.0f};
    float scale{1.0f};

    float mapLength(float logicalLength) const noexcept {
        return logicalLength * scale;
    }

    graphics::PointF mapPoint(graphics::PointF localPoint) const noexcept {
        return localToGlobal.mapPoint(localPoint);
    }

    graphics::PointF unmapPoint(graphics::PointF globalPoint) const noexcept {
        return globalToLocal.mapPoint(globalPoint);
    }

    graphics::RectF mapRect(const graphics::RectF& localRect) const noexcept {
        return localToGlobal.mapRect(localRect);
    }

    bool containsGlobalPoint(float x, float y, float outset = 0.0f) const noexcept {
        const auto local = unmapPoint({x, y});
        return local.x >= localBounds.x - outset &&
               local.x < localBounds.x + localBounds.width + outset &&
               local.y >= localBounds.y - outset &&
               local.y < localBounds.y + localBounds.height + outset;
    }
};

inline WindowGroupTransform makeWindowGroupTransform(const Window& window,
                                                     float unscaledTitleHeight,
                                                     float requestedScale) noexcept {
    WindowGroupTransform group{};
    group.scale = std::clamp(requestedScale, 0.80f, 1.20f);

    const auto presentation = presentedBounds(window);
    group.localBounds = {
        0.0f, 0.0f,
        std::max(1.0f, presentation.width),
        std::max(1.0f, presentation.height),
    };

    const float scaledWidth = group.localBounds.width * group.scale;
    const float scaledHeight = group.localBounds.height * group.scale;
    float globalX = presentation.x + (presentation.width - scaledWidth) * 0.5f;
    float globalY = presentation.y + (presentation.height - scaledHeight) * 0.5f;

    // Exact resting frames remain pixel-aligned and therefore crisp. During
    // scale or geometry motion, preserve subpixel presentation coordinates so
    // the GPU sampler can blend movement instead of stepping whole pixels.
    if (!window.isMorphing() && std::fabs(group.scale - 1.0f) < 0.0001f) {
        globalX = std::round(globalX);
        globalY = std::round(globalY);
    }

    group.localToGlobal = graphics::Matrix3::scale(group.scale, group.scale)
        .followedBy(graphics::Matrix3::translation(globalX, globalY));
    group.globalToLocal = group.localToGlobal.inverted().value_or(
        graphics::Matrix3::identity());
    group.globalBounds = group.localToGlobal.mapRect(group.localBounds);
    group.titleHeight = std::clamp(
        std::max(0.0f, unscaledTitleHeight) * group.scale,
        0.0f, group.globalBounds.height);
    return group;
}

/** Map a complete WindowGroup into an explicit compositor presentation rect. */
inline WindowGroupTransform makeWindowGroupTransformToBounds(
        const Window& window, float unscaledTitleHeight,
        const graphics::RectF& globalBounds) noexcept {
    WindowGroupTransform group{};
    const auto presentation = presentedBounds(window);
    group.localBounds = {
        0.0f, 0.0f,
        std::max(1.0f, presentation.width),
        std::max(1.0f, presentation.height),
    };
    group.globalBounds = {
        globalBounds.x,
        globalBounds.y,
        std::max(1.0f, globalBounds.width),
        std::max(1.0f, globalBounds.height),
    };
    const float scaleX = group.globalBounds.width / group.localBounds.width;
    const float scaleY = group.globalBounds.height / group.localBounds.height;
    group.scale = std::min(scaleX, scaleY);
    group.localToGlobal = graphics::Matrix3::scale(scaleX, scaleY)
        .followedBy(graphics::Matrix3::translation(
            group.globalBounds.x, group.globalBounds.y));
    group.globalToLocal = group.localToGlobal.inverted().value_or(
        graphics::Matrix3::identity());
    group.titleHeight = std::clamp(
        std::max(0.0f, unscaledTitleHeight) * scaleY,
        0.0f, group.globalBounds.height);
    return group;
}

} // namespace lcl::render
