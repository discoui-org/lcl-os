#pragma once

#include <algorithm>
#include <cstdint>

#include "core/ipc/lcl_protocol.hpp"

namespace lcl::core {

struct LocalEffectGeometry {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

inline LocalEffectGeometry resolveLocalEffectGeometry(
    int windowX,
    int windowY,
    int titleOffset,
    uint32_t surfaceWidth,
    uint32_t surfaceHeight,
    const protocol::EffectRegion& region,
    bool followSurfaceBounds) {
    return {
        windowX + (followSurfaceBounds ? 0 : region.x),
        windowY + titleOffset + (followSurfaceBounds ? 0 : region.y),
        followSurfaceBounds ? static_cast<int>(surfaceWidth)
                            : static_cast<int>(region.width),
        followSurfaceBounds ? static_cast<int>(surfaceHeight)
                            : static_cast<int>(region.height),
    };
}

} // namespace lcl::core
