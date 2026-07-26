#include "lcl-ui/widgets/container.hpp"

namespace lcl::ui {

Container::Container() = default;

void Container::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    // Draw container children
    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
