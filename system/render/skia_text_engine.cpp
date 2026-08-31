#include "system/render/skia_text_engine.hpp"

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

#include <algorithm>
#include <cmath>

namespace lcl::render::skia_text {
namespace {

struct TypefaceCache {
    sk_sp<SkFontMgr> manager{SkFontMgr_New_Custom_Empty()};
    sk_sp<SkTypeface> interfaceTypeface;
    sk_sp<SkTypeface> monospaceTypeface;
    sk_sp<SkTypeface> iconsTypeface;

    sk_sp<SkTypeface> typeface(lcl::graphics::FontFamily family) {
        sk_sp<SkTypeface>* cached = &interfaceTypeface;
        if (family == lcl::graphics::FontFamily::Monospace) {
            cached = &monospaceTypeface;
        } else if (family == lcl::graphics::FontFamily::Icons) {
            cached = &iconsTypeface;
        }
        if (*cached) return *cached;
        const auto path = text_metrics::resolveFontPath(family);
        if (!path || !manager) return nullptr;
        *cached = manager->makeFromFile(path->c_str());
        return *cached;
    }
};

TypefaceCache& cache() {
    thread_local TypefaceCache value;
    return value;
}

void configureFont(SkFont& font) {
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kNone);
    font.setLinearMetrics(true);
    font.setSubpixel(true);
}

text_metrics::TextMetrics metricsFor(const SkFont& font) {
    SkFontMetrics raw{};
    font.getMetrics(&raw);
    const float lineHeight = std::max(
        0.0f, raw.fDescent - raw.fAscent + raw.fLeading);
    return {
        .advanceWidth = 0.0f,
        .ascent = -raw.fAscent,
        .descent = raw.fDescent,
        .leading = raw.fLeading,
        .lineHeight = lineHeight,
    };
}

} // namespace

std::optional<PreparedFont> prepareFont(
        lcl::graphics::FontFamily family, float logicalPixelHeight) {
    if (!std::isfinite(logicalPixelHeight)) return std::nullopt;
    auto typeface = cache().typeface(family);
    if (!typeface) return std::nullopt;

    SkFont unitFont(typeface, 1.0f);
    configureFont(unitFont);
    const auto unitMetrics = metricsFor(unitFont);
    const float unitHeight = unitMetrics.ascent + unitMetrics.descent;
    if (!std::isfinite(unitHeight) || unitHeight <= 0.0001f) {
        return std::nullopt;
    }

    const float requestedHeight = std::max(1.0f, logicalPixelHeight);
    SkFont font(std::move(typeface), requestedHeight / unitHeight);
    configureFont(font);
    return PreparedFont{font, metricsFor(font)};
}

text_metrics::TextMetrics measureText(
        const std::string& text, float logicalPixelHeight,
        lcl::graphics::FontFamily family) {
    const auto prepared = prepareFont(family, logicalPixelHeight);
    if (!prepared) return {};
    auto result = prepared->metrics;
    if (!text.empty()) {
        result.advanceWidth = prepared->font.measureText(
            text.data(), text.size(), SkTextEncoding::kUTF8);
    }
    return result;
}

} // namespace lcl::render::skia_text
