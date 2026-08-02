#include "lcl-ui/widgets/text.hpp"
#include "render/skia_renderer.hpp"

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
    if (!m_visible || !m_absoluteBounds.intersects(damageRect) || m_text.empty()) return;

    auto* renderer = reinterpret_cast<::lcl::render::SkiaRenderer*>(canvas);
    if (renderer) {
        // Keep text alignment in logical coordinates; SkiaRenderer performs the
        // one-and-only logical-to-buffer transform afterwards.
        const float textWidth = renderer->measureString(m_text, m_fontSize);
        float textX = m_absoluteBounds.x;
        if (m_textAlign == TextAlign::Center) {
            textX += std::max(0.0f, (m_absoluteBounds.width - textWidth) * 0.5f);
        } else if (m_textAlign == TextAlign::End) {
            textX += std::max(0.0f, m_absoluteBounds.width - textWidth);
        }
        int textY = static_cast<int>(m_absoluteBounds.y);
        uint32_t argbColor = (static_cast<uint32_t>(m_textColor.a) << 24) |
                             (static_cast<uint32_t>(m_textColor.r) << 16) |
                             (static_cast<uint32_t>(m_textColor.g) << 8)  |
                             static_cast<uint32_t>(m_textColor.b);
        renderer->drawString(static_cast<int>(textX), textY, m_text, argbColor, m_fontSize);
    }
}

} // namespace lcl::ui
