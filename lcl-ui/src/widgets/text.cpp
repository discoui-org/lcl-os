#include "lcl-ui/widgets/text.hpp"
#include "render/text_metrics.hpp"

#include <algorithm>

namespace lcl::ui {

Text::Text(const std::string& content) : m_text(content) {
    updateMeasureFunc();
    styleDidChange();
}

void Text::setText(const std::string& text) {
    m_text = text;
    updateMeasureFunc();
    markDirty();
}

void Text::setFontSize(float size) {
    m_fontSize = size;
    updateMeasureFunc();
    markDirty();
}

void Text::setFontFamily(graphics::FontFamily family) {
    if (m_fontFamily == family) return;
    m_fontFamily = family;
    updateMeasureFunc();
    markDirty();
}

void Text::setTextColor(const graphics::Color& color) {
    m_hasExplicitTextColor = true;
    m_textColor = color;
    markDirty();
}

void Text::resetTextColor() {
    if (!m_hasExplicitTextColor) return;
    m_hasExplicitTextColor = false;
    styleDidChange();
}

const lcl::theme::WidgetStyle* Text::defaultStyle() const noexcept {
    return &getTheme().text;
}

void Text::styleDidChange() {
    if (m_hasExplicitTextColor) return;
    const auto* style = resolvedStyle();
    if (!style) return;
    m_textColor = lcl::theme::resolveStyle(
        *style, lcl::theme::StyleState::Normal).foreground;
    markDirty();
}

void Text::updateMeasureFunc() {
    m_yogaNode.setMeasureFunc([this](float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
        (void)width; (void)widthMode; (void)height; (void)heightMode;
        const float measuredW = lcl::render::text_metrics::measureText(
            m_text, m_fontSize, m_fontFamily);
        float measuredH = m_fontSize * 1.2f;
        return YGSize{measuredW, measuredH};
    });
    m_yogaNode.markDirty();
}

void Text::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect) || m_text.empty()) return;

    beginPresentation(canvas);

    // Keep text alignment in logical coordinates; graphics::Canvas applies the selected
    // backend's logical-to-buffer transform afterwards.
    const float textWidth = canvas.measureText(m_text, m_fontSize, m_fontFamily);
    float textX = m_absoluteBounds.x;
    if (m_textAlign == TextAlign::Center) {
        textX += std::max(0.0f, (m_absoluteBounds.width - textWidth) * 0.5f);
    } else if (m_textAlign == TextAlign::End) {
        textX += std::max(0.0f, m_absoluteBounds.width - textWidth);
    }
    if (hasActiveAnimationInHierarchy()) {
        // Keep one glyph raster stable for the entire transform. Once the
        // animation settles the normal path below is used again, producing a
        // fresh, pixel-aligned final render instead of scaling forever.
        canvas.drawRasterizedText(textX, m_absoluteBounds.y, m_text, m_textColor,
                                  m_fontSize, m_fontFamily);
    } else {
        canvas.drawText(textX, m_absoluteBounds.y, m_text, m_textColor,
                        m_fontSize, m_fontFamily);
    }
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
