#include "lcl-ui/widgets/backdrop_surface.hpp"

#include <algorithm>

namespace lcl::ui {

BackdropSurface::BackdropSurface() {
    // Keep default fully transparent; demos/apps can opt in to tint explicitly.
    setBackgroundColor(Color{255, 255, 255, 0});
    setFocusable(true);
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

bool BackdropSurface::onPointerEnter(const PointerEvent& event) {
    (void)event;
    return true;
}

bool BackdropSurface::onPointerLeave(const PointerEvent& event) {
    (void)event;
    m_pressed = false;
    return true;
}

bool BackdropSurface::onPointerDown(const PointerEvent& event) {
    (void)event;
    m_pressed = true;
    return true;
}

bool BackdropSurface::onPointerUp(const PointerEvent& event) {
    (void)event;
    bool wasPressed = m_pressed;
    m_pressed = false;
    if (wasPressed && m_onClick) {
        m_onClick();
    }
    return true;
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
