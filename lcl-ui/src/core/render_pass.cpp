#include "lcl-ui/core/render_pass.hpp"

namespace lcl::ui {

void RenderPass::addMergedDirtyRect(
        std::vector<graphics::RectF>& regions,
        const graphics::RectF& rect) {
    if (rect.isEmpty()) return;

    graphics::RectF merged = rect;
    for (auto it = regions.begin(); it != regions.end();) {
        if (!merged.intersects(*it)) {
            ++it;
            continue;
        }
        merged = merged.unionWith(*it);
        it = regions.erase(it);
    }
    regions.push_back(merged);

    // Keep traversal bounded under pathological invalidation storms. Normal
    // UI motion remains a small list of independent regions.
    constexpr size_t kMaxDamageRegions = 32;
    if (regions.size() > kMaxDamageRegions) {
        graphics::RectF combined = regions.front();
        for (std::size_t index = 1; index < regions.size(); ++index) {
            combined = combined.unionWith(regions[index]);
        }
        regions.assign(1, combined);
    }
}

void RenderPass::addDirtyRect(const graphics::RectF& rect) {
    addMergedDirtyRect(m_dirtyRects, rect);
    addMergedDirtyRect(m_rasterDirtyRects, rect);
}

void RenderPass::addCompositingDirtyRect(const graphics::RectF& rect) {
    addMergedDirtyRect(m_dirtyRects, rect);
}

void RenderPass::clear() {
    m_dirtyRects.clear();
    m_rasterDirtyRects.clear();
    m_frameDamageRects.clear();
    m_frameRasterDamageRects.clear();
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

void RenderPass::begin(
        graphics::Canvas& canvas,
        const std::vector<graphics::RectF>& frameDamageRects) {
    begin(canvas, frameDamageRects, frameDamageRects);
}

void RenderPass::begin(
        graphics::Canvas& canvas,
        const std::vector<graphics::RectF>& frameDamageRects,
        const std::vector<graphics::RectF>& frameRasterDamageRects) {
    (void)canvas;
    m_frameDamageRects = frameDamageRects;
    m_frameRasterDamageRects = frameRasterDamageRects;
    ++m_passSerial;
    if (m_passSerial == 0) ++m_passSerial;
    m_inPass = true;
}

void RenderPass::end(graphics::Canvas& canvas) {
    (void)canvas;
    m_inPass = false;
    m_frameDamageRects.clear();
    m_frameRasterDamageRects.clear();
}

} // namespace lcl::ui
