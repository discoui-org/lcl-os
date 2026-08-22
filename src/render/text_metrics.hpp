#pragma once

#include "lcl-graphics/font.hpp"
#include "render/font_renderer.hpp"

#include <string>

namespace lcl::render::text_metrics {

/** Loads the same packaged face selected by the client renderer. */
bool loadFont(FontRenderer& renderer, lcl::graphics::FontFamily family, float pixelFontSize);

/** Returns the exact glyph-advance width used by the renderer for this face. */
float measureText(const std::string& text, float fontSize,
                  lcl::graphics::FontFamily family = lcl::graphics::FontFamily::Interface);

} // namespace lcl::render::text_metrics
