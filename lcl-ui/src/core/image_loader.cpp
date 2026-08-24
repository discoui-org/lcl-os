#include "lcl-ui/core/image_loader.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include "render/stb_image.h"

namespace lcl::ui {

namespace {

std::atomic<uint64_t> nextImageResourceId{1};
std::mutex sharedImageCacheMutex;
std::unordered_map<std::string, std::weak_ptr<const ImageData>> sharedImageCache;

uint64_t allocateImageResourceId() {
    return nextImageResourceId.fetch_add(1, std::memory_order_relaxed);
}

std::string normalizedImagePath(const std::string& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path : canonical.string();
}

} // namespace

std::optional<ImageData> ImageLoader::loadArgb32(const std::string& path) {
    int imgW = 0;
    int imgH = 0;
    int channels = 0;
    unsigned char* raw = stbi_load(path.c_str(), &imgW, &imgH, &channels, 4);
    if (!raw || imgW <= 0 || imgH <= 0) {
        if (raw) stbi_image_free(raw);
        return std::nullopt;
    }

    ImageData out{};
    out.resourceId = allocateImageResourceId();
    out.width = static_cast<uint32_t>(imgW);
    out.height = static_cast<uint32_t>(imgH);
    out.pixels.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));
    out.opaque = true;

    for (size_t i = 0; i < out.pixels.size(); ++i) {
        const unsigned char* p = raw + i * 4;
        const uint32_t a = static_cast<uint32_t>(p[3]);
        const uint32_t r = static_cast<uint32_t>(p[0]);
        const uint32_t g = static_cast<uint32_t>(p[1]);
        const uint32_t b = static_cast<uint32_t>(p[2]);
        out.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
        out.opaque = out.opaque && a == 0xFFu;
    }

    stbi_image_free(raw);
    return out;
}

std::shared_ptr<const ImageData> ImageLoader::loadSharedArgb32(
        const std::string& path) {
    if (path.empty()) return {};
    const std::string key = normalizedImagePath(path);
    {
        std::lock_guard lock(sharedImageCacheMutex);
        if (const auto found = sharedImageCache.find(key);
            found != sharedImageCache.end()) {
            if (auto cached = found->second.lock()) return cached;
            sharedImageCache.erase(found);
        }
    }

    auto decoded = loadArgb32(key);
    if (!decoded) return {};
    auto resource = std::make_shared<const ImageData>(std::move(*decoded));
    {
        std::lock_guard lock(sharedImageCacheMutex);
        const auto [found, inserted] = sharedImageCache.try_emplace(key, resource);
        if (!inserted) {
            if (auto cached = found->second.lock()) return cached;
            found->second = resource;
        }
    }
    return resource;
}

ImageData ImageLoader::resizeBilinear(const ImageData& src, uint32_t targetWidth, uint32_t targetHeight) {
    ImageData out{};
    out.resourceId = allocateImageResourceId();
    out.width = targetWidth;
    out.height = targetHeight;

    if (!src.isValid() || targetWidth == 0 || targetHeight == 0) {
        return out;
    }

    out.pixels.resize(static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight));
    out.opaque = true;

    auto unpack = [](uint32_t argb, float& a, float& r, float& g, float& b) {
        a = static_cast<float>((argb >> 24) & 0xFF);
        r = static_cast<float>((argb >> 16) & 0xFF);
        g = static_cast<float>((argb >> 8) & 0xFF);
        b = static_cast<float>(argb & 0xFF);
    };

    auto lerp = [](float x0, float x1, float t) {
        return x0 + (x1 - x0) * t;
    };

    for (uint32_t y = 0; y < targetHeight; ++y) {
        float v = (static_cast<float>(y) + 0.5f) * (static_cast<float>(src.height) / static_cast<float>(targetHeight)) - 0.5f;
        int y0 = std::clamp(static_cast<int>(std::floor(v)), 0, static_cast<int>(src.height) - 1);
        int y1 = std::clamp(y0 + 1, 0, static_cast<int>(src.height) - 1);
        float fy = v - std::floor(v);

        for (uint32_t x = 0; x < targetWidth; ++x) {
            float u = (static_cast<float>(x) + 0.5f) * (static_cast<float>(src.width) / static_cast<float>(targetWidth)) - 0.5f;
            int x0 = std::clamp(static_cast<int>(std::floor(u)), 0, static_cast<int>(src.width) - 1);
            int x1 = std::clamp(x0 + 1, 0, static_cast<int>(src.width) - 1);
            float fx = u - std::floor(u);

            float a00, r00, g00, b00;
            float a01, r01, g01, b01;
            float a10, r10, g10, b10;
            float a11, r11, g11, b11;

            unpack(src.pixels[static_cast<size_t>(y0) * src.width + static_cast<size_t>(x0)], a00, r00, g00, b00);
            unpack(src.pixels[static_cast<size_t>(y0) * src.width + static_cast<size_t>(x1)], a01, r01, g01, b01);
            unpack(src.pixels[static_cast<size_t>(y1) * src.width + static_cast<size_t>(x0)], a10, r10, g10, b10);
            unpack(src.pixels[static_cast<size_t>(y1) * src.width + static_cast<size_t>(x1)], a11, r11, g11, b11);

            float a = lerp(lerp(a00, a01, fx), lerp(a10, a11, fx), fy);
            float r = lerp(lerp(r00, r01, fx), lerp(r10, r11, fx), fy);
            float g = lerp(lerp(g00, g01, fx), lerp(g10, g11, fx), fy);
            float b = lerp(lerp(b00, b01, fx), lerp(b10, b11, fx), fy);

            uint32_t au = static_cast<uint32_t>(std::clamp(a, 0.0f, 255.0f));
            uint32_t ru = static_cast<uint32_t>(std::clamp(r, 0.0f, 255.0f));
            uint32_t gu = static_cast<uint32_t>(std::clamp(g, 0.0f, 255.0f));
            uint32_t bu = static_cast<uint32_t>(std::clamp(b, 0.0f, 255.0f));

            out.pixels[static_cast<size_t>(y) * targetWidth + static_cast<size_t>(x)] =
                (au << 24) | (ru << 16) | (gu << 8) | bu;
            out.opaque = out.opaque && au == 0xFFu;
        }
    }

    return out;
}

} // namespace lcl::ui
