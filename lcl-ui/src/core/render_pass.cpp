#include "lcl-ui/core/render_pass.hpp"

namespace lcl::ui {

void RenderPass::addDirtyRect(const Rect& rect) {
    if (!rect.isEmpty()) {
        m_dirtyRects.push_back(rect);
    }
}

void RenderPass::clear() {
    m_dirtyRects.clear();
    m_inPass = false;
}

Rect RenderPass::getDamageRect() const {
    if (m_dirtyRects.empty()) {
        return Rect{0.0f, 0.0f, 0.0f, 0.0f};
    }
    Rect damage = m_dirtyRects[0];
    for (size_t i = 1; i < m_dirtyRects.size(); ++i) {
        damage = damage.unionWith(m_dirtyRects[i]);
    }
    return damage;
}

void RenderPass::begin(Canvas& canvas) {
    (void)canvas;
    m_inPass = true;
}

void RenderPass::end(Canvas& canvas) {
    (void)canvas;
    m_inPass = false;
}

} // namespace lcl::ui
