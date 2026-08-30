#include "platforms/common/output_scale.hpp"

#include <cmath>

namespace lcl::platform {

float sanitizeOutputScale(float scale) noexcept {
    if (!std::isfinite(scale) || scale < 0.5f) {
        return 1.0f;
    }
    if (scale > 4.0f) {
        return 4.0f;
    }

    constexpr float commonScales[]{
        1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f,
    };
    for (const float candidate : commonScales) {
        if (std::fabs(scale - candidate) < 0.08f) {
            return candidate;
        }
    }
    return std::round(scale * 100.0f) / 100.0f;
}

} // namespace lcl::platform
