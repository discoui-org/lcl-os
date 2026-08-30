#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lcl::ui {

struct ImageData {
    uint64_t resourceId{0};
    uint64_t contentRevision{1};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint32_t> pixels; // ARGB32
    bool opaque{false};

    bool isValid() const {
        return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

class ImageLoader {
public:
    // Load an image file using stb_image and convert to ARGB32.
    static std::optional<ImageData> loadArgb32(const std::string& path);

    /** Share immutable decoded pixels between Image widgets using the same path. */
    static std::shared_ptr<const ImageData> loadSharedArgb32(
        const std::string& path);

    // Bilinear resize to target dimensions, preserving ARGB32 format.
    static ImageData resizeBilinear(const ImageData& src, uint32_t targetWidth, uint32_t targetHeight);
};

} // namespace lcl::ui
