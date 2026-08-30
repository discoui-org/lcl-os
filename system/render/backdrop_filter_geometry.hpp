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
    int maskWidth{0};
    int maskHeight{0};
    int maskOffsetX{0};
    int maskOffsetY{0};
    bool clippedLeft{false};
    bool clippedRight{false};
    bool clippedTop{false};
    bool clippedBottom{false};
};

struct BackdropPassScale {
    float x{1.0f};
    float y{1.0f};
};

struct BackdropBlurPlan {
    float gaussianValuePx{0.0f};
    float downsampleScale{1.0f};
};

/**
 * Resolves a genuine variable-radius Gaussian backdrop blur in device pixels.
 * The working scale changes continuously with the requested radius so an
 * animation does not cross discrete full/half/quarter-resolution cliffs.
 * No sharp/filtered opacity blend participates in the visual result.
 */
inline BackdropBlurPlan computeBackdropBlurPlan(float requestedValuePx) {
    if (!std::isfinite(requestedValuePx) || requestedValuePx <= 0.05f) {
        return {0.0f, 1.0f};
    }

    const float requested = std::max(0.0f, requestedValuePx);
    constexpr float kFullResolutionRadiusPx = 8.0f;
    const float downsampleScale = std::clamp(
        requested / kFullResolutionRadiusPx, 1.0f, 4.0f);
    return {requested, downsampleScale};
}

/** Logical-to-pass pixel scale after an optional intermediate downsample. */
inline BackdropPassScale computeBackdropPassScale(
    float deviceScale,
    int targetWidth,
    int targetHeight,
    int captureWidth,
    int captureHeight) {
    const float safeDeviceScale = std::isfinite(deviceScale) && deviceScale > 0.0f
        ? deviceScale
        : 1.0f;
    return {
        safeDeviceScale * static_cast<float>(std::max(1, targetWidth)) /
            static_cast<float>(std::max(1, captureWidth)),
        safeDeviceScale * static_cast<float>(std::max(1, targetHeight)) /
            static_cast<float>(std::max(1, captureHeight)),
    };
}

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

    // Keep the rounded mask anchored to the original effect rect. Clipping to
    // the framebuffer exposes only a slice; it must not manufacture new
    // corners at the screen edge.
    geometry.maskWidth = effectWidth;
    geometry.maskHeight = effectHeight;
    geometry.maskOffsetX = geometry.effect.x - effectX;
    geometry.maskOffsetY = geometry.effect.y - effectY;
    geometry.clippedLeft = effectX < 0;
    geometry.clippedRight = effectX + effectWidth > sceneWidth;
    geometry.clippedTop = effectY < 0;
    geometry.clippedBottom = effectY + effectHeight > sceneHeight;

    // Capture strictly the surface effect boundary. External scene pixels
    // are not sampled to eliminate pre-entry blur halo bleeding.
    geometry.capture = geometry.effect;
    geometry.outputOffsetX = 0;
    geometry.outputOffsetY = 0;
    return geometry;
}

} // namespace lcl::render
