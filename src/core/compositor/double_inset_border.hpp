#pragma once

#include "render/skia_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lcl::core {

struct DoubleInsetBorderGeometry {
    render::SkiaRect outerBounds{};
    render::SkiaRect innerBounds{};
    float outerRadius{0.0f};
    float innerRadius{0.0f};
    float strokeWidth{0.0f};
    bool hasInnerBorder{false};
};

inline DoubleInsetBorderGeometry resolveDoubleInsetBorderGeometry(
        const render::SkiaRect& bounds, float restingRadiusPx,
        float displayScale, float presentationScale) noexcept {
    const float scale = std::max(0.0f, presentationScale);
    const float strokeWidth = std::max(0.0f, displayScale) * scale;
    const float innerWidth = std::max(0.0f, bounds.width - strokeWidth * 2.0f);
    const float innerHeight = std::max(0.0f, bounds.height - strokeWidth * 2.0f);
    const float outerRadius = std::max(0.0f, restingRadiusPx * scale);

    return {
        bounds,
        {bounds.x + strokeWidth, bounds.y + strokeWidth, innerWidth, innerHeight},
        outerRadius,
        std::max(0.0f, outerRadius - strokeWidth),
        strokeWidth,
        strokeWidth > 0.0f && innerWidth > 0.0f && innerHeight > 0.0f,
    };
}

inline uint8_t doubleInsetBorderAlpha(uint8_t alpha, float opacity) noexcept {
    const float scaled = std::clamp(
        static_cast<float>(alpha) * std::clamp(opacity, 0.0f, 1.0f),
        0.0f, 255.0f);
    return static_cast<uint8_t>(std::lround(scaled));
}

inline void drawDoubleInsetBorder(
        render::SkiaRenderer& renderer, const render::SkiaRect& bounds,
        float restingRadiusPx, float roundness, float opacity,
        float displayScale, float presentationScale) {
    const auto geometry = resolveDoubleInsetBorderGeometry(
        bounds, restingRadiusPx, displayScale, presentationScale);
    if (geometry.strokeWidth <= 0.0f) return;

    renderer.drawRoundedRect(
        geometry.outerBounds,
        geometry.outerRadius,
        {0, 0, 0, 0},
        {10, 12, 16, doubleInsetBorderAlpha(120, opacity)},
        geometry.strokeWidth,
        roundness);

    if (!geometry.hasInnerBorder) return;
    renderer.drawRoundedRect(
        geometry.innerBounds,
        geometry.innerRadius,
        {0, 0, 0, 0},
        {245, 248, 252, doubleInsetBorderAlpha(86, opacity)},
        geometry.strokeWidth,
        roundness);
}

} // namespace lcl::core
