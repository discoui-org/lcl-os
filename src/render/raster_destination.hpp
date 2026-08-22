#pragma once

namespace lcl::render {

struct DeviceRasterDestination {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float cornerRadius{0.0f};
};

/** Convert a logical destination to physical framebuffer coordinates.
 * Source and backing buffer extents deliberately do not participate here. */
inline DeviceRasterDestination mapLogicalRasterDestination(
        float x, float y, float width, float height, float cornerRadius,
        float deviceScale, float physicalOriginX = 0.0f,
        float physicalOriginY = 0.0f) noexcept {
    return {
        x * deviceScale + physicalOriginX,
        y * deviceScale + physicalOriginY,
        width * deviceScale,
        height * deviceScale,
        cornerRadius * deviceScale,
    };
}

} // namespace lcl::render
