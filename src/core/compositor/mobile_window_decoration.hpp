#pragma once

#include <algorithm>
#include <cmath>

#include "lcl-graphics/display_list.hpp"

namespace lcl::core {

struct MobileGesturePillMetrics {
    float referenceWidth{393.0f};
    float width{134.0f};
    float height{5.0f};
    float bottomInset{8.0f};
    lcl::graphics::Color color{255, 255, 255, 235};
};

struct MobileGesturePillLayout {
    lcl::graphics::RectF bounds{};
    float scale{1.0f};
};

inline MobileGesturePillLayout layoutMobileGesturePill(
        const lcl::graphics::RectF& windowBounds,
        const MobileGesturePillMetrics& metrics = {}) noexcept {
    const float referenceWidth = std::max(1.0f, metrics.referenceWidth);
    const float scale = std::max(0.0f, windowBounds.width) / referenceWidth;
    const float width = std::min(
        std::max(0.0f, windowBounds.width),
        std::max(0.0f, metrics.width * scale));
    const float height = std::min(
        std::max(0.0f, windowBounds.height),
        std::max(0.0f, metrics.height * scale));
    const float bottomInset = std::max(0.0f, metrics.bottomInset * scale);
    return {
        {
            windowBounds.x + (windowBounds.width - width) * 0.5f,
            windowBounds.y + std::max(
                0.0f, windowBounds.height - bottomInset - height),
            width,
            height,
        },
        scale,
    };
}

inline lcl::graphics::DisplayList buildMobileGesturePillDisplayList(
        const lcl::graphics::RectF& windowBounds,
        float opacity,
        const MobileGesturePillMetrics& metrics = {}) {
    lcl::graphics::DisplayListBuilder builder;
    const auto layout = layoutMobileGesturePill(windowBounds, metrics);
    if (layout.bounds.width <= 0.0f || layout.bounds.height <= 0.0f ||
        opacity <= 0.0f) {
        return builder.build();
    }

    auto color = metrics.color;
    color.a = static_cast<uint8_t>(std::clamp(std::lround(
        static_cast<float>(color.a) * std::clamp(opacity, 0.0f, 1.0f)),
        0l, 255l));
    lcl::graphics::Path path;
    const float radius = layout.bounds.height * 0.5f;
    path.addRRect({layout.bounds, radius, radius, 2.0f});
    builder.drawPath(path, {{color}, lcl::graphics::PaintStyle::Fill});
    return builder.build();
}

} // namespace lcl::core
