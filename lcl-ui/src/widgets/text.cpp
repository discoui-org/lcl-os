#include "lcl-ui/widgets/text.hpp"

namespace lcl::ui {

Text::Text(const std::string& content) : m_text(content) {
    updateMeasureFunc();
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

void Text::updateMeasureFunc() {
    m_yogaNode.setMeasureFunc([this](float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
        (void)width; (void)widthMode; (void)height; (void)heightMode;
        float charWidth = m_fontSize * 0.6f;
        float measuredW = static_cast<float>(m_text.length()) * charWidth;
        float measuredH = m_fontSize * 1.2f;
        return YGSize{measuredW, measuredH};
    });
}

void Text::draw(Canvas& canvas, const Rect& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect) || m_text.empty()) return;

    beginPresentation(canvas);

    // Keep text alignment in logical coordinates; Canvas applies the selected
    // backend's logical-to-buffer transform afterwards.
    const float textWidth = canvas.measureText(m_text, m_fontSize);
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
        canvas.drawRasterizedText(textX, m_absoluteBounds.y, m_text, m_textColor, m_fontSize);
    } else {
        canvas.drawText(textX, m_absoluteBounds.y, m_text, m_textColor, m_fontSize);
    }
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
