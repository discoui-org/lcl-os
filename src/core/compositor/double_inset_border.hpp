#pragma once

#include "lcl-graphics/display_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lcl::core {

inline uint8_t doubleInsetBorderAlpha(uint8_t alpha, float opacity) noexcept {
    const float scaled = std::clamp(
        static_cast<float>(alpha) * std::clamp(opacity, 0.0f, 1.0f),
        0.0f, 255.0f);
    return static_cast<uint8_t>(std::lround(scaled));
}

/** Build compositor chrome in logical space; the replay CTM owns all scaling. */
inline graphics::DisplayList buildDoubleInsetBorderDisplayList(
        const graphics::RectF& bounds, float radius, float roundness,
        float opacity) {
    graphics::DisplayListBuilder builder;
    if (bounds.isEmpty() || opacity <= 0.0f) return builder.build();

    const auto strokePath = [&](const graphics::RectF& rect, float pathRadius,
                                graphics::Color color) {
        if (rect.isEmpty()) return;
        graphics::Path path;
        path.addRRect({rect, std::max(0.0f, pathRadius),
                      std::max(0.0f, pathRadius),
                      std::clamp(roundness, 2.0f, 8.0f)});
        graphics::Paint paint;
        paint.color = color;
        paint.style = graphics::PaintStyle::Stroke;
        paint.stroke.width = 1.0f;
        paint.stroke.join = graphics::StrokeJoin::Round;
        builder.drawPath(path, paint);
    };

    strokePath(bounds, radius,
               {10, 12, 16, doubleInsetBorderAlpha(120, opacity)});
    const graphics::RectF inner{bounds.x + 1.0f, bounds.y + 1.0f,
                                std::max(0.0f, bounds.width - 2.0f),
                                std::max(0.0f, bounds.height - 2.0f)};
    strokePath(inner, radius - 1.0f,
               {245, 248, 252, doubleInsetBorderAlpha(86, opacity)});
    return builder.build();
}

} // namespace lcl::core
