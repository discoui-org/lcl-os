#include "lcl-ui/widgets/divider.hpp"

#include "lcl-graphics/canvas.hpp"

namespace lcl::ui {

layout::Size Divider::measure(const layout::Constraints& constraints) {
    (void)constraints;
    return {1.0f, 1.0f};
}

void Divider::draw(graphics::Canvas& canvas,
                   const graphics::RectF& damageRect) {
    if (!m_visible || isCollapsed() || !getPresentationBounds().intersects(damageRect)) return;
    beginPresentation(canvas, damageRect);
    const float width = getTheme().metrics.separatorWidth;
    if (m_absoluteBounds.width >= m_absoluteBounds.height) {
        canvas.drawRect({m_absoluteBounds.x,
                         m_absoluteBounds.y +
                             (m_absoluteBounds.height - width) * 0.5f,
                         m_absoluteBounds.width, width},
                        m_color.value_or(getTheme().colors.separator));
    } else {
        canvas.drawRect({m_absoluteBounds.x +
                             (m_absoluteBounds.width - width) * 0.5f,
                         m_absoluteBounds.y, width, m_absoluteBounds.height},
                        m_color.value_or(getTheme().colors.separator));
    }
    endPresentation(canvas);
}

} // namespace lcl::ui
