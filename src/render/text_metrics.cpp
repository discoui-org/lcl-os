#include "render/text_metrics.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <vector>

namespace lcl::render::text_metrics {
namespace {

bool isReadableFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

FontRenderer& cachedRenderer(lcl::graphics::FontFamily family) {
    thread_local FontRenderer interfaceFont;
    thread_local FontRenderer monospaceFont;
    return family == lcl::graphics::FontFamily::Monospace ? monospaceFont : interfaceFont;
}

} // namespace

std::optional<std::string> resolveFontPath(lcl::graphics::FontFamily family) {
    const bool monospace = family == lcl::graphics::FontFamily::Monospace;
    const std::string_view fileName = monospace
        ? "JetBrainsMono-Regular.ttf"
        : "Inter-Regular.otf";

    std::vector<std::string> candidates;
    if (const char* fontRoot = std::getenv("LCL_FONT_ROOT");
        fontRoot != nullptr && *fontRoot != '\0') {
        std::string path(fontRoot);
        if (path.back() != '/') path.push_back('/');
        path.append(fileName);
        candidates.push_back(std::move(path));
    }

    if (monospace) {
        candidates.emplace_back(
            "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf");
        candidates.emplace_back(
            "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf");
    } else {
        candidates.emplace_back("/usr/share/fonts/inter/Inter-Regular.otf");
        candidates.emplace_back("assets/fonts/inter/Inter-Regular.otf");
    }

    const auto found = std::find_if(candidates.begin(), candidates.end(), isReadableFile);
    if (found == candidates.end()) return std::nullopt;
    return *found;
}

bool loadFont(FontRenderer& renderer, lcl::graphics::FontFamily family, float pixelFontSize) {
    const float sanitizedSize = std::max(1.0f, pixelFontSize);
    const auto path = resolveFontPath(family);
    return path && renderer.loadFont(*path, sanitizedSize);
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
