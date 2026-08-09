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

            // Fast path for rectangular boxes: avoid expensive rounded-rect AA sampling.
            if (m_borderRadius <= 0.0f) {
                if (hasBorder) {
                    renderer->drawRect(r, bc);
                }

                if (hasBackground) {
                    if (hasBorder && m_borderWidth > 0.0f) {
                        const float inset = m_borderWidth;
                        const float innerW = std::max(0.0f, r.width - inset * 2.0f);
                        const float innerH = std::max(0.0f, r.height - inset * 2.0f);
                        if (innerW > 0.0f && innerH > 0.0f) {
                            renderer->drawRect({r.x + inset, r.y + inset, innerW, innerH}, c);
                        }
                    } else {
                        renderer->drawRect(r, c);
                    }
                }
            } else if (m_topOnlyBorderRadius && hasBackground && !hasBorder) {
                renderer->drawTopRoundedRect(r, m_borderRadius, c, m_borderRoundness);
            } else {
                renderer->drawRoundedRect(r, m_borderRadius, c, bc,
                                          hasBorder ? m_borderWidth : 0.0f,
                                          m_borderRoundness);
            }
        }
    }

    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
