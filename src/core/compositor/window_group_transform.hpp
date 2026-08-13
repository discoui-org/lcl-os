#pragma once

#include <algorithm>
#include <cmath>

#include "render/window_manager.hpp"

namespace lcl::core {

/** One shared presentation transform for every visual part of a compositor window group. */
struct WindowGroupTransform {
    float x{0.0f};
    float y{0.0f};
    float width{1.0f};
    float height{1.0f};
    float titleHeight{0.0f};
    float scale{1.0f};
};

inline WindowGroupTransform makeWindowGroupTransform(const render::Window& window,
                                                     int unscaledTitleHeight,
                                                     float requestedScale) noexcept {
    WindowGroupTransform group{};
    group.scale = std::clamp(requestedScale, 0.80f, 1.20f);
    const float presentationX = window.presentationInitialized ? window.presentationX : static_cast<float>(window.x);
    const float presentationY = window.presentationInitialized ? window.presentationY : static_cast<float>(window.y);
    const float presentationWidth = window.presentationInitialized ? window.presentationWidth : static_cast<float>(window.width);
    const float presentationHeight = window.presentationInitialized ? window.presentationHeight : static_cast<float>(window.height);
    group.width = std::max(1.0f, presentationWidth * group.scale);
    group.height = std::max(1.0f, presentationHeight * group.scale);
    group.x = presentationX + (presentationWidth - group.width) * 0.5f;
    group.y = presentationY + (presentationHeight - group.height) * 0.5f;
    group.titleHeight = std::clamp(
        static_cast<float>(std::max(0, unscaledTitleHeight)) * group.scale,
        0.0f, group.height);

    // Exact resting frames remain pixel-aligned and therefore crisp. During
    // scale or geometry motion, preserve subpixel presentation coordinates so
    // the GPU sampler can blend movement instead of stepping whole pixels.
    if (!window.geometryTransitionActive && std::fabs(group.scale - 1.0f) < 0.0001f) {
        group.x = std::round(group.x);
        group.y = std::round(group.y);
        group.width = std::round(group.width);
        group.height = std::round(group.height);
        group.titleHeight = std::round(group.titleHeight);
    }
    return group;
}

} // namespace lcl::core
