#include "render/text_metrics.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace lcl::render::text_metrics {
namespace {

template <size_t N>
bool loadFirstAvailable(FontRenderer& renderer, const char* const (&paths)[N], float pixelFontSize) {
    for (const char* path : paths) {
        if (renderer.loadFont(path, pixelFontSize)) return true;
    }
    return false;
}

FontRenderer& cachedRenderer(lcl::graphics::FontFamily family) {
    thread_local FontRenderer interfaceFont;
    thread_local FontRenderer monospaceFont;
    return family == lcl::graphics::FontFamily::Monospace ? monospaceFont : interfaceFont;
}

} // namespace

bool loadFont(FontRenderer& renderer, lcl::graphics::FontFamily family, float pixelFontSize) {
    const float sanitizedSize = std::max(1.0f, pixelFontSize);
    if (family == lcl::graphics::FontFamily::Monospace) {
        constexpr const char* kMonospacePaths[] = {
            "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
            "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        };
        return loadFirstAvailable(renderer, kMonospacePaths, sanitizedSize);
    }

    constexpr const char* kInterfacePaths[] = {
        "/usr/share/fonts/inter/Inter-Regular.otf",
        "assets/fonts/inter/Inter-Regular.otf",
    };
    return loadFirstAvailable(renderer, kInterfacePaths, sanitizedSize);
}

float measureText(const std::string& text, float fontSize, lcl::graphics::FontFamily family) {
    if (text.empty() || !std::isfinite(fontSize)) return 0.0f;

    FontRenderer& renderer = cachedRenderer(family);
    const float sanitizedSize = std::max(1.0f, fontSize);
    if (!renderer.isInitialized() ||
        std::fabs(renderer.getFontSize() - sanitizedSize) > 0.01f) {
        renderer = FontRenderer{};
        if (!loadFont(renderer, family, sanitizedSize)) return 0.0f;
    }
    return static_cast<float>(renderer.getTextWidth(text));
}

} // namespace lcl::render::text_metrics
