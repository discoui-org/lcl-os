#pragma once

#include <algorithm>
#include <cmath>

namespace lcl::render {

struct BackdropIntRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

struct BackdropFilterGeometry {
    BackdropIntRect effect{};
    BackdropIntRect capture{};
    int outputOffsetX{0};
    int outputOffsetY{0};
};

inline int gaussianKernelRadius(float blurValue) {
    if (!std::isfinite(blurValue) || blurValue <= 0.05f) return 0;
    const float sigma = std::max(0.5f, blurValue * 0.5f);
    return std::clamp(static_cast<int>(std::ceil(3.0f * sigma)), 1, 128);
}

inline BackdropFilterGeometry computeBackdropFilterGeometry(
    int effectX,
    int effectY,
    int effectWidth,
    int effectHeight,
    int sceneWidth,
    int sceneHeight,
    int /*kernelRadius*/ = 0) {
    BackdropFilterGeometry geometry{};
    if (effectWidth <= 0 || effectHeight <= 0 || sceneWidth <= 0 || sceneHeight <= 0) {
        return geometry;
    }

    const int effectRight = std::clamp(effectX + effectWidth, 0, sceneWidth);
    const int effectBottom = std::clamp(effectY + effectHeight, 0, sceneHeight);
    geometry.effect.x = std::clamp(effectX, 0, sceneWidth);
    geometry.effect.y = std::clamp(effectY, 0, sceneHeight);
    geometry.effect.width = std::max(0, effectRight - geometry.effect.x);
    geometry.effect.height = std::max(0, effectBottom - geometry.effect.y);
    if (geometry.effect.width == 0 || geometry.effect.height == 0) return geometry;

    // Capture strictly the surface effect boundary. External scene pixels
    // are not sampled to eliminate pre-entry blur halo bleeding.
    geometry.capture = geometry.effect;
    geometry.outputOffsetX = 0;
    geometry.outputOffsetY = 0;
    return geometry;
}

} // namespace lcl::render
