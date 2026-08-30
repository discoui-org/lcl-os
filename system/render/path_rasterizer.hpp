#pragma once

#include <cstdint>
#include <vector>

#include "lcl-graphics/path.hpp"

namespace lcl::render {

struct RasterizedPath {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    std::vector<uint32_t> pixels;

    bool empty() const noexcept {
        return width <= 0 || height <= 0 || pixels.empty();
    }
};

RasterizedPath rasterizePath(const lcl::graphics::Path& path,
                             const lcl::graphics::Paint& paint,
                             const lcl::graphics::Matrix3& deviceTransform,
                             float inheritedOpacity = 1.0f);

} // namespace lcl::render

