#include "lcl-ui/widgets/backdrop_surface.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {

namespace {

static uint8_t addClamped(uint8_t v, int delta) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(v) + delta, 0, 255));
}

static graphics::Color adjustColor(const graphics::Color& c, int dr, int dg, int db) {
    return graphics::Color{
        addClamped(c.r, dr),
        addClamped(c.g, dg),
        addClamped(c.b, db),
        c.a
    };
}

} // namespace

BackdropSurface::BackdropSurface() {
    // Keep default fully transparent; demos/apps can opt in to tint explicitly.
    setBackgroundColor(graphics::Color{255, 255, 255, 0});
    setFocusable(true);
}

void BackdropSurface::setFilters(const std::vector<lcl::protocol::FilterOp>& filters) {
    m_filters = filters;
    m_tint = {0, 0, 0, 0};
    for (const auto& filter : m_filters) {
        if (filter.type != lcl::protocol::FilterType::Tint) continue;
        m_tint = {
            static_cast<uint8_t>(std::clamp(std::lround(filter.params[0]), 0l, 255l)),
            static_cast<uint8_t>(std::clamp(std::lround(filter.params[1]), 0l, 255l)),
            static_cast<uint8_t>(std::clamp(std::lround(filter.params[2]), 0l, 255l)),
            static_cast<uint8_t>(std::clamp(std::lround(filter.value * 255.0f), 0l, 255l)),
        };
    }
    invalidatePaint();
}

void BackdropSurface::addFilter(lcl::protocol::FilterType type, float value,
                                float parameter1, float parameter2) {
    lcl::protocol::FilterOp op{};
    op.type = type;
    op.value = value;

    if (type == lcl::protocol::FilterType::Glass) {
        op.value = 1.0f;
        op.profile = static_cast<uint8_t>(lcl::protocol::GlassProfile::Auto);
        op.params[0] = std::max(0.0f, value);
        op.params[1] = std::max(0.0f, parameter1);
        op.params[2] = std::max(0.0f, parameter2);
    }

    addFilter(op);
}

void BackdropSurface::addFilter(const lcl::protocol::FilterOp& filter) {
    m_filters.push_back(filter);
    invalidatePaint();
}

void BackdropSurface::clearFilters() {
    if (m_filters.empty() && m_tint.a == 0) return;
    m_filters.clear();
    m_tint = {0, 0, 0, 0};
    invalidatePaint();
}

void BackdropSurface::setTint(const graphics::Color& color) {
    m_filters.erase(
        std::remove_if(m_filters.begin(), m_filters.end(), [](const auto& filter) {
            return filter.type == lcl::protocol::FilterType::Tint;
        }),
        m_filters.end());

    m_tint = color;
    if (color.a > 0) {
        lcl::protocol::FilterOp tint{};
        tint.type = lcl::protocol::FilterType::Tint;
        tint.value = static_cast<float>(color.a) / 255.0f;
        tint.params[0] = static_cast<float>(color.r);
        tint.params[1] = static_cast<float>(color.g);
        tint.params[2] = static_cast<float>(color.b);
        m_filters.push_back(tint);
    }
    invalidatePaint();
}

void BackdropSurface::setOpacity(float opacity) {
    const float next = std::clamp(opacity, 0.0f, 1.0f);
    if (m_opacity == next) return;
    m_opacity = next;
    invalidatePaint();
}

void BackdropSurface::setInteractive(bool interactive) {
    if (m_interactive == interactive) return;
    m_interactive = interactive;
    setFocusable(interactive);
    if (!m_interactive) {
        m_pressed = false;
        m_hovered = false;
        restoreBaseVisuals();
    }
}

bool BackdropSurface::onPointerEnter(const PointerEvent& event) {
    (void)event;
    if (!m_interactive) return false;
    if (!m_hasBaseVisuals) {
        m_baseBackground = getBackgroundColor();
        m_baseBorder = getBorderColor();
        m_hasBaseVisuals = true;
    }
    if (!m_pressed) {
        m_hovered = true;
        applyHoverVisuals();
    }
    return true;
}

bool BackdropSurface::onPointerLeave(const PointerEvent& event) {
    (void)event;
    if (!m_interactive) return false;
    m_pressed = false;
    m_hovered = false;
    restoreBaseVisuals();
    return true;
}

bool BackdropSurface::onPointerDown(const PointerEvent& event) {
    (void)event;
    if (!m_interactive) return false;
    if (!m_hasBaseVisuals) {
        m_baseBackground = getBackgroundColor();
        m_baseBorder = getBorderColor();
        m_hasBaseVisuals = true;
    }
    m_pressed = true;
    applyPressedVisuals();
    return true;
}

bool BackdropSurface::onPointerUp(const PointerEvent& event) {
    if (!m_interactive) return false;
    bool wasPressed = m_pressed;
    m_pressed = false;
    m_hovered = (event.source == PointerSource::Mouse);
    if (m_hasBaseVisuals) {
        if (m_hovered) {
            applyHoverVisuals();
        } else {
            restoreBaseVisuals();
        }
    }
    if (wasPressed && m_onClick) {
        m_onClick();
    }
    return true;
}

bool BackdropSurface::onPointerCancel(const PointerEvent& event) {
    (void)event;
    if (!m_interactive) return false;
    m_pressed = false;
    m_hovered = false;
    restoreBaseVisuals();
    return true;
}

void BackdropSurface::applyHoverVisuals() {
    const auto& theme = interactionMotionTheme();
    const graphics::Color background = adjustColor(m_baseBackground, 8, 8, 9);
    const graphics::Color border = adjustColor(m_baseBorder, 24, 27, 31);
    if (!theme.enabled || !m_motionCoordinator) {
        setBackgroundColor(background); setBorderColor(border); setScale(theme.hoverScale); return;
    }
    animateBackgroundColor(background, theme.focusTransition);
    animateBorderColor(border, theme.focusTransition);
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX, m_presentation.scaleX,
        theme.hoverScale, theme.hover, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY, m_presentation.scaleY,
        theme.hoverScale, theme.hover, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
}

void BackdropSurface::applyPressedVisuals() {
    const auto& theme = interactionMotionTheme();
    const graphics::Color background = adjustColor(m_baseBackground, 16, 16, 20);
    const graphics::Color border = adjustColor(m_baseBorder, 50, 53, 60);
    if (!theme.enabled || !m_motionCoordinator) {
        setBackgroundColor(background); setBorderColor(border); setScale(theme.pressedScale); return;
    }
    animateBackgroundColor(background, theme.focusTransition);
    animateBorderColor(border, theme.focusTransition);
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX, m_presentation.scaleX,
        theme.pressedScale, theme.pressed, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY, m_presentation.scaleY,
        theme.pressedScale, theme.pressed, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
}

void BackdropSurface::restoreBaseVisuals() {
    if (!m_hasBaseVisuals) return;
    const auto& theme = interactionMotionTheme();
    if (!theme.enabled || !m_motionCoordinator) {
        setBackgroundColor(m_baseBackground); setBorderColor(m_baseBorder); setScale(1.0f); return;
    }
    animateBackgroundColor(m_baseBackground, theme.focusTransition);
    animateBorderColor(m_baseBorder, theme.focusTransition);
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX, m_presentation.scaleX,
        1.0f, theme.release, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY, m_presentation.scaleY,
        1.0f, theme.release, [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
}

bool BackdropSurface::onFocusGained(const FocusEvent& event) {
    (void)event;
    m_focused = true;
    if (m_interactive && !m_hovered && !m_pressed) applyHoverVisuals();
    return false;
}

bool BackdropSurface::onFocusLost(const FocusEvent& event) {
    (void)event;
    m_focused = false;
    if (m_interactive && !m_hovered && !m_pressed) restoreBaseVisuals();
    return false;
}

void BackdropSurface::collectEffects(std::vector<EffectRegion>& outEffects) const {
    if (isVisible() && !m_filters.empty()) {
        const graphics::RectF abs = getAbsoluteBounds();
        if (!abs.isEmpty()) {
            EffectRegion region;
            region.bounds = abs;
            region.cornerRadius = getBorderRadius();
            region.cornerRoundness = getBorderRoundness();
            region.boundsPolicy = m_effectBounds;
            region.source = EffectSource::SurfaceBackdrop;
            region.blend = m_blendMode;
            region.opacity = m_opacity;
            region.filters = m_filters;
            outEffects.push_back(std::move(region));
        }
    }

    Container::collectEffects(outEffects);
}

} // namespace lcl::ui
