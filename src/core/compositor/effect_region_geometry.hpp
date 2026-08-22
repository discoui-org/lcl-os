#pragma once

#include <algorithm>
#include <cstdint>

#include "core/ipc/lcl_protocol.hpp"

namespace lcl::core {

struct LocalEffectGeometry {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

inline LocalEffectGeometry resolveLocalEffectGeometry(
    float windowX,
    float windowY,
    float titleOffset,
    float surfaceWidth,
    float surfaceHeight,
    const protocol::EffectRegion& region,
    bool followSurfaceBounds) {
    return {
        windowX + (followSurfaceBounds ? 0.0f : region.x),
        windowY + titleOffset + (followSurfaceBounds ? 0.0f : region.y),
        followSurfaceBounds ? surfaceWidth : region.width,
        followSurfaceBounds ? surfaceHeight : region.height,
    };
}

} // namespace lcl::core
