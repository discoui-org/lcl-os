#pragma once

#include <algorithm>
#include <cstdint>

namespace lcl::render::alpha {

constexpr uint8_t premultiplyChannel(uint8_t channel, uint8_t alpha) noexcept {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(channel) * alpha + 127u) / 255u);
}

constexpr uint8_t unpremultiplyChannel(uint8_t channel, uint8_t alpha) noexcept {
    if (alpha == 0) return 0;
    return static_cast<uint8_t>(std::min(
        255u,
        (static_cast<uint32_t>(channel) * 255u + alpha / 2u) / alpha));
}

/** Convert the public CPU/SHM straight-ARGB representation to GPU premultiplied ARGB. */
constexpr uint32_t premultiplyArgb(uint32_t pixel) noexcept {
    const auto a = static_cast<uint8_t>(pixel >> 24u);
    const auto r = static_cast<uint8_t>(pixel >> 16u);
    const auto g = static_cast<uint8_t>(pixel >> 8u);
    const auto b = static_cast<uint8_t>(pixel);
    return (static_cast<uint32_t>(a) << 24u) |
           (static_cast<uint32_t>(premultiplyChannel(r, a)) << 16u) |
           (static_cast<uint32_t>(premultiplyChannel(g, a)) << 8u) |
           static_cast<uint32_t>(premultiplyChannel(b, a));
}

/** Convert a GPU readback to the public CPU/SHM straight-ARGB representation. */
constexpr uint32_t unpremultiplyArgb(uint32_t pixel) noexcept {
    const auto a = static_cast<uint8_t>(pixel >> 24u);
    const auto r = static_cast<uint8_t>(pixel >> 16u);
    const auto g = static_cast<uint8_t>(pixel >> 8u);
    const auto b = static_cast<uint8_t>(pixel);
    return (static_cast<uint32_t>(a) << 24u) |
           (static_cast<uint32_t>(unpremultiplyChannel(r, a)) << 16u) |
           (static_cast<uint32_t>(unpremultiplyChannel(g, a)) << 8u) |
           static_cast<uint32_t>(unpremultiplyChannel(b, a));
}

} // namespace lcl::render::alpha
