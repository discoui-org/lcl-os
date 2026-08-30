#include "system/render/text_metrics.hpp"
#include "system/render/skia_text_engine.hpp"

#include <algorithm>
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

TextMetrics measure(const std::string& text, float fontSize,
                    lcl::graphics::FontFamily family) {
    return skia_text::measureText(text, fontSize, family);
}

float measureText(const std::string& text, float fontSize, lcl::graphics::FontFamily family) {
    return measure(text, fontSize, family).advanceWidth;
}

} // namespace lcl::render::text_metrics
