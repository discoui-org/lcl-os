#include "lcl-ui/core/render_pass.hpp"

namespace lcl::ui {

void RenderPass::addDirtyRect(const graphics::RectF& rect) {
    if (rect.isEmpty()) return;

    graphics::RectF merged = rect;
    for (auto it = m_dirtyRects.begin(); it != m_dirtyRects.end();) {
        if (!merged.intersects(*it)) {
            ++it;
            continue;
        }
        merged = merged.unionWith(*it);
        it = m_dirtyRects.erase(it);
    }
    m_dirtyRects.push_back(merged);

    // Keep traversal bounded under pathological invalidation storms. Normal
    // UI motion remains a small list of independent regions.
    constexpr size_t kMaxDamageRegions = 32;
    if (m_dirtyRects.size() > kMaxDamageRegions) {
        const graphics::RectF combined = getDamageRect();
        m_dirtyRects.assign(1, combined);
    }
}

void RenderPass::clear() {
    m_dirtyRects.clear();
    m_inPass = false;
}

graphics::RectF RenderPass::getDamageRect() const {
    if (m_dirtyRects.empty()) {
        return graphics::RectF{0.0f, 0.0f, 0.0f, 0.0f};
    }
    graphics::RectF damage = m_dirtyRects[0];
    for (size_t i = 1; i < m_dirtyRects.size(); ++i) {
        damage = damage.unionWith(m_dirtyRects[i]);
    }
    return damage;
}

void RenderPass::begin(graphics::Canvas& canvas) {
    (void)canvas;
    m_inPass = true;
}

void RenderPass::end(graphics::Canvas& canvas) {
    (void)canvas;
    m_inPass = false;
}

} // namespace lcl::ui
