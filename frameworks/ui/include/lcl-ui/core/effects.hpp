#pragma once

#include "lcl-graphics/geometry.hpp"
#include "system/ipc/lcl_protocol.hpp"

#include <vector>

namespace lcl::ui {

enum class EffectSource {
    Backdrop,
    Foreground
};

enum class EffectBlend {
    Normal,
    Screen,
    Multiply,
    Overlay,
    Plus
};

enum class EffectBounds {
    Local,
    OuterSurface
};

struct EffectRegion {
    graphics::RectF bounds;
    float cornerRadius{0.0f};
    float cornerRoundness{2.0f};
    EffectBounds boundsPolicy{EffectBounds::Local};
    EffectSource source{EffectSource::Backdrop};
    EffectBlend blend{EffectBlend::Normal};
    float opacity{1.0f};
    std::vector<lcl::protocol::FilterOp> filters;
};

} // namespace lcl::ui
