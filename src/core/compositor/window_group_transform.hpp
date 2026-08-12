#pragma once

#include <algorithm>
#include <cmath>

#include "render/window_manager.hpp"

namespace lcl::core {

/** One quantized transform for every visual part of a compositor window group. */
struct WindowGroupTransform {
    int x{0};
    int y{0};
    int width{1};
    int height{1};
    int titleHeight{0};
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
    group.width = std::max(1, static_cast<int>(std::lround(presentationWidth * group.scale)));
    group.height = std::max(1, static_cast<int>(std::lround(presentationHeight * group.scale)));
    group.x = static_cast<int>(std::lround(
        presentationX + (presentationWidth - group.width) * 0.5f));
    group.y = static_cast<int>(std::lround(
        presentationY + (presentationHeight - group.height) * 0.5f));
    group.titleHeight = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(std::max(0, unscaledTitleHeight)) * group.scale)),
        0, group.height);
    return group;
}

} // namespace lcl::core
