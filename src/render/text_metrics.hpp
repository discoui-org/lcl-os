#pragma once

#include "lcl-graphics/font.hpp"

#include <optional>
#include <string>

namespace lcl::render::text_metrics {

struct TextMetrics {
    float advanceWidth{0.0f};
    float ascent{0.0f};
    float descent{0.0f};
    float leading{0.0f};
    float lineHeight{0.0f};
};

/** Resolves the packaged face shared by measurement and raster replay. */
std::optional<std::string> resolveFontPath(lcl::graphics::FontFamily family);

/** Returns all Skia metrics used by layout and raster replay. */
TextMetrics measure(const std::string& text, float fontSize,
                    lcl::graphics::FontFamily family =
                        lcl::graphics::FontFamily::Interface);

/** Convenience accessor for the canonical Skia glyph advance. */
float measureText(const std::string& text, float fontSize,
                  lcl::graphics::FontFamily family = lcl::graphics::FontFamily::Interface);

} // namespace lcl::render::text_metrics
