#pragma once

#include <algorithm>
#include <cstdint>

namespace lcl::render {

struct DmaBufCrop {
    float uMax{1.0f};
    float vMax{1.0f};
};

/** Normalized lower-left crop for content rendered into a capacity-sized GL BO. */
inline DmaBufCrop makeDmaBufCrop(uint32_t contentWidth, uint32_t contentHeight,
                                 uint32_t backingWidth, uint32_t backingHeight) noexcept {
    const uint32_t safeBackingWidth = std::max(contentWidth, backingWidth);
    const uint32_t safeBackingHeight = std::max(contentHeight, backingHeight);
    if (safeBackingWidth == 0 || safeBackingHeight == 0) return {};
    return {
        std::clamp(static_cast<float>(contentWidth) / safeBackingWidth, 0.0f, 1.0f),
        std::clamp(static_cast<float>(contentHeight) / safeBackingHeight, 0.0f, 1.0f),
    };
}

} // namespace lcl::render
