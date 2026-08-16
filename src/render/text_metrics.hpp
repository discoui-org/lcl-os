#pragma once

#include "lcl-ui/core/font_family.hpp"
#include "render/font_renderer.hpp"

#include <string>

namespace lcl::render::text_metrics {

/** Loads the same packaged face selected by the client renderer. */
bool loadFont(FontRenderer& renderer, lcl::ui::FontFamily family, float pixelFontSize);

/** Returns the exact glyph-advance width used by the renderer for this face. */
float measureText(const std::string& text, float fontSize,
                  lcl::ui::FontFamily family = lcl::ui::FontFamily::Interface);

} // namespace lcl::render::text_metrics
