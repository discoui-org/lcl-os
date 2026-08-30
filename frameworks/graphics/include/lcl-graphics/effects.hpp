#pragma once

#include <cstdint>

namespace lcl::graphics {

/** Backend-neutral effect payload recorded in a DisplayList. */
enum class EffectType : uint8_t {
    None = 0,
    Blur = 1,
    Brightness = 2,
    Contrast = 3,
    Saturation = 4,
    Grayscale = 5,
    Invert = 6,
    Glass = 7,
    Tint = 8,
};

struct EffectOp {
    EffectType type{EffectType::None};
    float value{0.0f};
    uint8_t profile{0};
    uint8_t reserved0{0};
    uint16_t reserved1{0};
    float params[3]{0.0f, 0.0f, 0.0f};
};

} // namespace lcl::graphics
