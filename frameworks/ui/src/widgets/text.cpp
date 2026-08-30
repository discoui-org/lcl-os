#include "lcl-ui/widgets/text.hpp"
#include "system/render/text_metrics.hpp"

#include <algorithm>

namespace lcl::ui {

Text::Text(const std::string& content) : m_text(content) {
    styleDidChange();
}

void Text::setText(const std::string& text) {
    if (m_text == text) return;
    m_text = text;
    invalidateMeasurement();
    invalidatePaint();
}

void Text::setFontSize(float size) {
    if (m_hasExplicitFontSize && m_fontSize == size) return;
    m_hasExplicitFontSize = true;
    m_fontSize = size;
    invalidateMeasurement();
    invalidatePaint();
}

void Text::resetFontSize() {
    if (!m_hasExplicitFontSize) return;
    m_hasExplicitFontSize = false;
    styleDidChange();
}

void Text::setTextRole(lcl::theme::TextRole role) {
    if (m_textRole == role) return;
    m_textRole = role;
    styleDidChange();
}

void Text::setFontFamily(graphics::FontFamily family) {
    if (m_fontFamily == family) return;
    m_fontFamily = family;
    invalidateMeasurement();
    invalidatePaint();
}

void Text::setTextColor(const graphics::Color& color) {
    if (m_hasExplicitTextColor && m_textColor.toARGB() == color.toARGB()) return;
    m_hasExplicitTextColor = true;
    m_textColor = color;
    invalidatePaint();
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
    const auto& typography = lcl::theme::typographyForRole(
        getTheme(), m_textRole);
    if (!m_hasExplicitFontSize) {
        m_fontSize = typography.fontSize;
        invalidateMeasurement();
    }
    if (!m_hasExplicitTextColor) {
        if (hasStyleOverride()) {
            const auto* style = resolvedStyle();
            m_textColor = lcl::theme::resolveStyle(
                *style, lcl::theme::StyleState::Normal).foreground;
        } else {
            m_textColor = typography.foreground;
        }
    }
    invalidatePaint();
}

layout::Size Text::measure(const layout::Constraints& constraints) {
    (void)constraints;
    const auto metrics = lcl::render::text_metrics::measure(
        m_text, m_fontSize, m_fontFamily);
    return {metrics.advanceWidth, metrics.lineHeight};
}

void Text::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect) || m_text.empty()) return;

    beginPresentation(canvas, damageRect);

    // Keep text alignment in logical coordinates; graphics::Canvas applies the selected
    // backend's logical-to-buffer transform afterwards.
    const float textWidth = canvas.measureText(m_text, m_fontSize, m_fontFamily);
    float textX = m_absoluteBounds.x;
    if (m_textAlign == TextAlign::Center) {
        textX += std::max(0.0f, (m_absoluteBounds.width - textWidth) * 0.5f);
    } else if (m_textAlign == TextAlign::End) {
        textX += std::max(0.0f, m_absoluteBounds.width - textWidth);
    }
    canvas.drawText(textX, m_absoluteBounds.y, m_text, m_textColor,
                    m_fontSize, m_fontFamily);
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
