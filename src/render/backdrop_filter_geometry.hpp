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
    int kernelRadius) {
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

    const int padding = std::max(0, kernelRadius);
    geometry.capture.x = std::max(0, geometry.effect.x - padding);
    geometry.capture.y = std::max(0, geometry.effect.y - padding);
    const int captureRight = std::min(sceneWidth, effectRight + padding);
    const int captureBottom = std::min(sceneHeight, effectBottom + padding);
    geometry.capture.width = captureRight - geometry.capture.x;
    geometry.capture.height = captureBottom - geometry.capture.y;
    geometry.outputOffsetX = geometry.effect.x - geometry.capture.x;
    geometry.outputOffsetY = geometry.effect.y - geometry.capture.y;
    return geometry;
}

} // namespace lcl::render
