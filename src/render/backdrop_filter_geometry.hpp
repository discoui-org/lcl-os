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
    float filteredMix{1.0f};
    int downsampleDivisor{1};
};

/**
 * Resolves a Gaussian backdrop blur in device pixels. A positive transition
 * floor keeps the final part of an animated blur on the first downsampled mip
 * and crossfades that result with the sharp retained scene. This avoids the
 * expensive half-resolution to full-resolution Gaussian handoff near zero.
 */
inline BackdropBlurPlan computeBackdropBlurPlan(
    float requestedValuePx,
    float transitionFloorPx = 0.0f) {
    if (!std::isfinite(requestedValuePx) || requestedValuePx <= 0.05f) {
        return {0.0f, 0.0f, 1};
    }

    const float requested = std::max(0.0f, requestedValuePx);
    const float floor = std::isfinite(transitionFloorPx)
        ? std::max(0.0f, transitionFloorPx)
        : 0.0f;
    const bool transitional = floor > 0.05f;
    const float gaussianValue = transitional
        ? std::max(requested, floor)
        : requested;

    int divisor = 1;
    if (gaussianValue > 8.0f) {
        divisor = std::clamp(
            1 + static_cast<int>(std::floor(std::log2(gaussianValue / 8.0f))),
            1, 4);
    }
    if (transitional) {
        divisor = std::max(divisor, 2);
    }

    const float filteredMix = transitional
        ? std::clamp(requested / floor, 0.0f, 1.0f)
        : 1.0f;
    return {gaussianValue, filteredMix, divisor};
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
