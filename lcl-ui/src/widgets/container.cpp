#include "lcl-ui/widgets/container.hpp"

namespace lcl::ui {

Container::Container() = default;

void Container::draw(Canvas& canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    const Rect rect{m_absoluteBounds.x, m_absoluteBounds.y, m_absoluteBounds.width, m_absoluteBounds.height};
    const bool hasBackground = (m_backgroundColor.a > 0);
    const bool hasBorder = (m_borderWidth > 0.0f && m_borderColor.a > 0);
    if (hasBackground || hasBorder) {

        // Fast path for rectangular boxes: avoid expensive rounded-rect AA sampling.
        if (m_borderRadius <= 0.0f) {
            if (hasBorder) {
                canvas.drawRect(rect, m_borderColor);
            }

            if (hasBackground) {
                if (hasBorder && m_borderWidth > 0.0f) {
                    const float inset = m_borderWidth;
                    const float innerW = std::max(0.0f, rect.width - inset * 2.0f);
                    const float innerH = std::max(0.0f, rect.height - inset * 2.0f);
                    if (innerW > 0.0f && innerH > 0.0f) {
                        canvas.drawRect({rect.x + inset, rect.y + inset, innerW, innerH}, m_backgroundColor);
                    }
                } else {
                    canvas.drawRect(rect, m_backgroundColor);
                }
            }
        } else if (m_topOnlyBorderRadius && hasBackground && !hasBorder) {
            canvas.drawTopRoundedRect(rect, m_borderRadius, m_backgroundColor, m_borderRoundness);
        } else {
            canvas.drawRoundedRect(rect, m_borderRadius, m_backgroundColor, m_borderColor,
                                   hasBorder ? m_borderWidth : 0.0f, m_borderRoundness);
        }
    }

    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
