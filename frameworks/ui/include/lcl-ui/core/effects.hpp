#pragma once

#include "lcl-graphics/geometry.hpp"
#include "system/ipc/lcl_protocol.hpp"

#include <vector>

namespace lcl::ui {

/** Public effect payload. It is wire-compatible with compositor FilterOp. */
using Effect = lcl::protocol::FilterOp;
using EffectType = lcl::protocol::FilterType;

enum class EffectSource {
    /** Filter this widget's own rendered layer. */
    Layer,
    /** Filter already-painted content in this application surface. */
    Backdrop,
    /** Filter the compositor scene behind this application's window surface. */
    SurfaceBackdrop,
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
    EffectSource source{EffectSource::SurfaceBackdrop};
    EffectBlend blend{EffectBlend::Normal};
    float opacity{1.0f};
    std::vector<lcl::protocol::FilterOp> filters;
};

} // namespace lcl::ui
