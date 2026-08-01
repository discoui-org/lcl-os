#include "lcl-ui/widgets/filter_group.hpp"

#include <algorithm>

namespace lcl::ui {

FilterGroup::FilterGroup() = default;

void FilterGroup::setFilters(const std::vector<lcl::protocol::FilterOp>& filters) {
    m_filters = filters;
    markDirty();
}

void FilterGroup::addFilter(lcl::protocol::FilterType type, float value) {
    m_filters.push_back({type, value});
    markDirty();
}

void FilterGroup::clearFilters() {
    m_filters.clear();
    markDirty();
}

void FilterGroup::setOpacity(float opacity) {
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    markDirty();
}

void FilterGroup::collectEffects(std::vector<EffectRegion>& outEffects) const {
    if (isVisible() && !m_filters.empty()) {
        const Rect abs = getAbsoluteBounds();
        if (!abs.isEmpty()) {
            EffectRegion region;
            region.bounds = abs;
            region.source = EffectSource::Foreground;
            region.blend = m_blendMode;
            region.opacity = m_opacity;
            region.filters = m_filters;
            outEffects.push_back(std::move(region));
        }
    }

    Container::collectEffects(outEffects);
}

} // namespace lcl::ui
