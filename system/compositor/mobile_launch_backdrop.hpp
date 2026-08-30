#pragma once

#include <algorithm>

namespace lcl::core {

/** Compositor-owned presentation values for the mobile launcher background. */
struct MobileLaunchBackdropPresentation {
    float progress{0.0f};
    float wallpaperScale{1.0f};
    float homeScale{1.0f};
    float brightness{1.0f};
};

inline MobileLaunchBackdropPresentation resolveMobileLaunchBackdrop(
        float progress) noexcept {
    constexpr float kWallpaperCoveredScale = 1.06f;
    constexpr float kHomeCoveredScale = 0.94f;
    constexpr float kCoveredBrightness = 0.82f;

    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    return {
        clamped,
        1.0f + (kWallpaperCoveredScale - 1.0f) * clamped,
        1.0f + (kHomeCoveredScale - 1.0f) * clamped,
        1.0f + (kCoveredBrightness - 1.0f) * clamped,
    };
}

} // namespace lcl::core
