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
    group.width = std::max(1, static_cast<int>(std::lround(static_cast<float>(window.width) * group.scale)));
    group.height = std::max(1, static_cast<int>(std::lround(static_cast<float>(window.height) * group.scale)));
    group.x = static_cast<int>(std::lround(
        static_cast<float>(window.x) + (static_cast<float>(window.width) - group.width) * 0.5f));
    group.y = static_cast<int>(std::lround(
        static_cast<float>(window.y) + (static_cast<float>(window.height) - group.height) * 0.5f));
    group.titleHeight = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(std::max(0, unscaledTitleHeight)) * group.scale)),
        0, group.height);
    return group;
}

} // namespace lcl::core
