#pragma once

#include "lcl-graphics/geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace lcl::core {

/**
 * Map damage expressed in producer-buffer pixels into a compositor logical
 * destination. The destination may differ from the buffer extent because DPR
 * is resolved before composition.
 */
inline std::optional<graphics::RectF> mapSurfaceDamageToDestination(
        uint32_t surfaceWidth, uint32_t surfaceHeight,
        uint32_t damageX, uint32_t damageY,
        uint32_t damageWidth, uint32_t damageHeight,
        const graphics::RectF& destination,
        const graphics::RectF& outputBounds,
        float edgeSafety = 2.0f) noexcept {
    if (surfaceWidth == 0 || surfaceHeight == 0 ||
        damageWidth == 0 || damageHeight == 0 || destination.isEmpty()) {
        return std::nullopt;
    }

    const uint64_t damageRight = std::min<uint64_t>(
        surfaceWidth, static_cast<uint64_t>(damageX) + damageWidth);
    const uint64_t damageBottom = std::min<uint64_t>(
        surfaceHeight, static_cast<uint64_t>(damageY) + damageHeight);
    const uint32_t clippedX = std::min(damageX, surfaceWidth);
    const uint32_t clippedY = std::min(damageY, surfaceHeight);
    if (damageRight <= clippedX || damageBottom <= clippedY) {
        return std::nullopt;
    }

    const float scaleX = destination.width /
        static_cast<float>(surfaceWidth);
    const float scaleY = destination.height /
        static_cast<float>(surfaceHeight);
    const float safety = std::max(0.0f, edgeSafety);
    const graphics::RectF mapped{
        destination.x + static_cast<float>(clippedX) * scaleX - safety,
        destination.y + static_cast<float>(clippedY) * scaleY - safety,
        static_cast<float>(damageRight - clippedX) * scaleX + safety * 2.0f,
        static_cast<float>(damageBottom - clippedY) * scaleY + safety * 2.0f,
    };
    const auto clipped = mapped.intersection(outputBounds);
    if (clipped.isEmpty()) return std::nullopt;
    return clipped;
}

} // namespace lcl::core
