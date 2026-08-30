#pragma once

#include <cstdint>

namespace lcl::graphics {

struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};

    constexpr uint32_t toARGB() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }
};

enum class FillRule : uint8_t { NonZero, EvenOdd };
enum class PaintStyle : uint8_t { Fill, Stroke };
enum class StrokeCap : uint8_t { Butt, Round, Square };
enum class StrokeJoin : uint8_t { Miter, Round, Bevel };
enum class StrokeScaling : uint8_t { ScaleWithTransform, Hairline };

struct StrokeStyle {
    float width{1.0f};
    StrokeCap cap{StrokeCap::Butt};
    StrokeJoin join{StrokeJoin::Miter};
    float miterLimit{4.0f};
    StrokeScaling scaling{StrokeScaling::ScaleWithTransform};
};

struct Paint {
    Color color{};
    PaintStyle style{PaintStyle::Fill};
    FillRule fillRule{FillRule::NonZero};
    StrokeStyle stroke{};
    float opacity{1.0f};
};

} // namespace lcl::graphics

