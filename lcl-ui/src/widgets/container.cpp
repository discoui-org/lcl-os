#include "lcl-ui/widgets/container.hpp"
#include "render/skia_renderer.hpp"

namespace lcl::ui {

Container::Container() = default;

void Container::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    auto* renderer = reinterpret_cast<::lcl::render::SkiaRenderer*>(canvas);
    if (renderer) {
        ::lcl::render::SkiaRect r{m_absoluteBounds.x, m_absoluteBounds.y, m_absoluteBounds.width, m_absoluteBounds.height};
        const bool hasBackground = (m_backgroundColor.a > 0);
        const bool hasBorder = (m_borderWidth > 0.0f && m_borderColor.a > 0);
        if (hasBackground || hasBorder) {
            ::lcl::render::SkiaColor c{m_backgroundColor.r, m_backgroundColor.g, m_backgroundColor.b, m_backgroundColor.a};
            ::lcl::render::SkiaColor bc{m_borderColor.r, m_borderColor.g, m_borderColor.b, m_borderColor.a};
            renderer->drawRoundedRect(r, m_borderRadius, c, bc, hasBorder ? m_borderWidth : 0.0f);
        }
    }

    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
