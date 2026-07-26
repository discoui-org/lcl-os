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

void Text::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;
    (void)canvas;
}

} // namespace lcl::ui
