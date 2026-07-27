#include "lcl-ui/widgets/container.hpp"
#include "render/skia_renderer.hpp"

namespace lcl::ui {

Container::Container() = default;

void Container::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    auto* renderer = reinterpret_cast<::lcl::render::SkiaRenderer*>(canvas);
    if (renderer) {
        ::lcl::render::SkiaRect r{m_absoluteBounds.x, m_absoluteBounds.y, m_absoluteBounds.width, m_absoluteBounds.height};
        if (m_backgroundColor.a > 0) {
            ::lcl::render::SkiaColor c{m_backgroundColor.r, m_backgroundColor.g, m_backgroundColor.b, m_backgroundColor.a};
            ::lcl::render::SkiaColor bc{m_borderColor.r, m_borderColor.g, m_borderColor.b, m_borderColor.a};
            renderer->drawRoundedRect(r, 12.0f, c, bc, m_borderWidth);
        } else {
            // Default card background styling for containers: dark slate navy `#1E293B`
            ::lcl::render::SkiaColor c{30, 41, 59, 240};
            ::lcl::render::SkiaColor bc{51, 65, 85, 255};
            renderer->drawRoundedRect(r, 12.0f, c, bc, 1.0f);
        }
    }

    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
