#include "lcl-ui/widgets/backdrop_surface.hpp"

#include <algorithm>

namespace lcl::ui {

BackdropSurface::BackdropSurface() {
    // Subtle frosted tint by default so backdrop effects are visible.
    setBackgroundColor(Color{255, 255, 255, 28});
}

void BackdropSurface::setFilters(const std::vector<lcl::protocol::FilterOp>& filters) {
    m_filters = filters;
    markDirty();
}

void BackdropSurface::addFilter(lcl::protocol::FilterType type, float value) {
    m_filters.push_back({type, value});
    markDirty();
}

void BackdropSurface::clearFilters() {
    m_filters.clear();
    markDirty();
}

void BackdropSurface::setOpacity(float opacity) {
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    markDirty();
}

void BackdropSurface::collectEffects(std::vector<EffectRegion>& outEffects) const {
    if (isVisible() && !m_filters.empty()) {
        const Rect abs = getAbsoluteBounds();
        if (!abs.isEmpty()) {
            EffectRegion region;
            region.bounds = abs;
            region.source = EffectSource::Backdrop;
            region.blend = m_blendMode;
            region.opacity = m_opacity;
            region.filters = m_filters;
            outEffects.push_back(std::move(region));
        }
    }

    Container::collectEffects(outEffects);
}

} // namespace lcl::ui
