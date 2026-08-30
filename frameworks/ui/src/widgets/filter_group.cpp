#include "lcl-ui/widgets/filter_group.hpp"

#include <algorithm>

namespace lcl::ui {

FilterGroup::FilterGroup() = default;

void FilterGroup::setFilters(const std::vector<lcl::protocol::FilterOp>& filters) {
    m_filters = filters;
    invalidatePaint();
}

void FilterGroup::addFilter(lcl::protocol::FilterType type, float value) {
    m_filters.push_back({type, value});
    invalidatePaint();
}

void FilterGroup::clearFilters() {
    if (m_filters.empty()) return;
    m_filters.clear();
    invalidatePaint();
}

void FilterGroup::setOpacity(float opacity) {
    const float next = std::clamp(opacity, 0.0f, 1.0f);
    if (m_opacity == next) return;
    m_opacity = next;
    invalidatePaint();
}

void FilterGroup::collectEffects(std::vector<EffectRegion>& outEffects) const {
    if (isVisible() && !m_filters.empty()) {
        const graphics::RectF abs = getAbsoluteBounds();
        if (!abs.isEmpty()) {
            EffectRegion region;
            region.bounds = abs;
            region.cornerRadius = getBorderRadius();
            region.cornerRoundness = getBorderRoundness();
            region.source = EffectSource::Layer;
            region.blend = m_blendMode;
            region.opacity = m_opacity;
            region.filters = m_filters;
            outEffects.push_back(std::move(region));
        }
    }

    Container::collectEffects(outEffects);
}

} // namespace lcl::ui
