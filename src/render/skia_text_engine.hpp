#pragma once

#include "lcl-graphics/font.hpp"
#include "render/text_metrics.hpp"

#include "include/core/SkFont.h"

#include <optional>
#include <string>

namespace lcl::render::skia_text {

struct PreparedFont {
    SkFont font;
    text_metrics::TextMetrics metrics;
};

/**
 * Creates the one canonical Skia font used by layout and raster replay.
 * LCL fontSize means logical ascent-to-descent pixel height, matching the
 * framework's pre-Skia visual contract rather than Skia's raw em size.
 */
std::optional<PreparedFont> prepareFont(
    lcl::graphics::FontFamily family, float logicalPixelHeight);

text_metrics::TextMetrics measureText(
    const std::string& text, float logicalPixelHeight,
    lcl::graphics::FontFamily family);

} // namespace lcl::render::skia_text
