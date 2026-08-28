#include "lcl-ui/widgets/container.hpp"

namespace lcl::ui {

Container::Container() = default;

graphics::RectF Container::getUntransformedPaintBounds() const noexcept {
    const bool hasStroke = m_presentationBorderWidth > 0.0f &&
        m_presentationBorderColor.a > 0;
    if (!hasStroke) return m_absoluteBounds;
    const float outset = m_presentationBorderWidth * 0.5f;
    return {
        m_absoluteBounds.x - outset,
        m_absoluteBounds.y - outset,
        m_absoluteBounds.width + outset * 2.0f,
        m_absoluteBounds.height + outset * 2.0f,
    };
}

void Container::styleDidChange() {
    const auto* style = resolvedStyle();
    if (!style) return;
    const auto visual = lcl::theme::resolveStyle(
        *style, lcl::theme::StyleState::Normal);
    setBackgroundColor(visual.background);
    setBorderColor(visual.border);
    setBorderWidth(visual.borderWidth);
    setBorderRadius(visual.cornerRadius);
    setPadding(layout::Edge::Horizontal, style->horizontalPadding.value_or(0.0f));
    setPadding(layout::Edge::Vertical, style->verticalPadding.value_or(0.0f));
}

void Container::setBackgroundColor(const graphics::Color& color) {
    m_backgroundColor = color;
    if (m_motionCoordinator) {
        m_motionCoordinator->setColor(*this, AnimatableProperty::BackgroundRed,
            m_presentationBackgroundColor, color,
            [this](graphics::Color next) { m_presentationBackgroundColor = next; markDirty(); });
    } else { m_presentationBackgroundColor = color; markDirty(); }
}

void Container::animateBackgroundColor(const graphics::Color& color, const lcl::motion::Motion& motion) {
    m_backgroundColor = color;
    if (!m_motionCoordinator) { m_presentationBackgroundColor = color; markDirty(); return; }
    m_motionCoordinator->setColor(*this, AnimatableProperty::BackgroundRed,
        m_presentationBackgroundColor, color,
        [this](graphics::Color next) { m_presentationBackgroundColor = next; markDirty(); }, &motion);
}

void Container::setBorderColor(const graphics::Color& color) {
    m_borderColor = color;
    const auto apply = [this](graphics::Color next) {
        const graphics::RectF previous = getVisiblePresentationPaintBounds();
        m_presentationBorderColor = next;
        markPaintDirty(previous);
    };
    if (m_motionCoordinator) {
        m_motionCoordinator->setColor(*this, AnimatableProperty::BorderRed,
            m_presentationBorderColor, color, apply);
    } else { apply(color); }
}

void Container::animateBorderColor(const graphics::Color& color, const lcl::motion::Motion& motion) {
    m_borderColor = color;
    const auto apply = [this](graphics::Color next) {
        const graphics::RectF previous = getVisiblePresentationPaintBounds();
        m_presentationBorderColor = next;
        markPaintDirty(previous);
    };
    if (!m_motionCoordinator) { apply(color); return; }
    m_motionCoordinator->setColor(*this, AnimatableProperty::BorderRed,
        m_presentationBorderColor, color, apply, &motion);
}

void Container::setBorderWidth(float width) {
    m_borderWidth = std::max(0.0f, width);
    const auto apply = [this](float next) {
        const graphics::RectF previous = getVisiblePresentationPaintBounds();
        m_presentationBorderWidth = next;
        markPaintDirty(previous);
    };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::BorderWidth,
        m_presentationBorderWidth, m_borderWidth, apply);
    else apply(m_borderWidth);
}

void Container::setBorderRadius(float radius) {
    m_borderRadius = std::max(0.0f, radius);
    const auto apply = [this](float next) { m_presentationBorderRadius = next; markDirty(); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::BorderRadius,
        m_presentationBorderRadius, m_borderRadius, apply);
    else apply(m_borderRadius);
}

float Container::getPresentationValue(AnimatableProperty property) const {
    switch (property) {
        case AnimatableProperty::BackgroundRed: return m_presentationBackgroundColor.r;
        case AnimatableProperty::BackgroundGreen: return m_presentationBackgroundColor.g;
        case AnimatableProperty::BackgroundBlue: return m_presentationBackgroundColor.b;
        case AnimatableProperty::BackgroundAlpha: return m_presentationBackgroundColor.a;
        case AnimatableProperty::BorderRed: return m_presentationBorderColor.r;
        case AnimatableProperty::BorderGreen: return m_presentationBorderColor.g;
        case AnimatableProperty::BorderBlue: return m_presentationBorderColor.b;
        case AnimatableProperty::BorderAlpha: return m_presentationBorderColor.a;
        case AnimatableProperty::BorderWidth: return m_presentationBorderWidth;
        case AnimatableProperty::BorderRadius: return m_presentationBorderRadius;
        default: return Widget::getPresentationValue(property);
    }
}

void Container::applyPresentationValue(AnimatableProperty property, float value) {
    const graphics::RectF previous = getVisiblePresentationPaintBounds();
    const auto byte = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
    switch (property) {
        case AnimatableProperty::BackgroundRed: m_presentationBackgroundColor.r = byte; break;
        case AnimatableProperty::BackgroundGreen: m_presentationBackgroundColor.g = byte; break;
        case AnimatableProperty::BackgroundBlue: m_presentationBackgroundColor.b = byte; break;
        case AnimatableProperty::BackgroundAlpha: m_presentationBackgroundColor.a = byte; break;
        case AnimatableProperty::BorderRed: m_presentationBorderColor.r = byte; break;
        case AnimatableProperty::BorderGreen: m_presentationBorderColor.g = byte; break;
        case AnimatableProperty::BorderBlue: m_presentationBorderColor.b = byte; break;
        case AnimatableProperty::BorderAlpha: m_presentationBorderColor.a = byte; break;
        case AnimatableProperty::BorderWidth: m_presentationBorderWidth = std::max(0.0f, value); break;
        case AnimatableProperty::BorderRadius: m_presentationBorderRadius = std::max(0.0f, value); break;
        default: Widget::applyPresentationValue(property, value); return;
    }
    markPaintDirty(previous);
}

void Container::commitModelValue(AnimatableProperty property, float value) {
    graphics::Color color;
    switch (property) {
        case AnimatableProperty::BackgroundRed: case AnimatableProperty::BackgroundGreen:
        case AnimatableProperty::BackgroundBlue: case AnimatableProperty::BackgroundAlpha:
            color = m_backgroundColor;
            if (property == AnimatableProperty::BackgroundRed) color.r = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BackgroundGreen) color.g = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BackgroundBlue) color.b = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BackgroundAlpha) color.a = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            setBackgroundColor(color); return;
        case AnimatableProperty::BorderRed: case AnimatableProperty::BorderGreen:
        case AnimatableProperty::BorderBlue: case AnimatableProperty::BorderAlpha:
            color = m_borderColor;
            if (property == AnimatableProperty::BorderRed) color.r = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BorderGreen) color.g = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BorderBlue) color.b = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            if (property == AnimatableProperty::BorderAlpha) color.a = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            setBorderColor(color); return;
        case AnimatableProperty::BorderWidth: setBorderWidth(value); return;
        case AnimatableProperty::BorderRadius: setBorderRadius(value); return;
        default: Widget::commitModelValue(property, value); return;
    }
}

void Container::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationPaintBounds().intersects(damageRect)) return;

    beginPresentation(canvas);

    const graphics::RectF rect{m_absoluteBounds.x, m_absoluteBounds.y, m_absoluteBounds.width, m_absoluteBounds.height};
    const bool hasBackground = (m_presentationBackgroundColor.a > 0);
    const bool hasBorder = (m_presentationBorderWidth > 0.0f && m_presentationBorderColor.a > 0);
    if (hasBackground || hasBorder) {

        // Fast path for rectangular boxes: avoid expensive rounded-rect AA sampling.
        if (m_presentationBorderRadius <= 0.0f) {
            if (hasBorder) {
                canvas.drawRect(rect, m_presentationBorderColor);
            }

            if (hasBackground) {
                if (hasBorder && m_presentationBorderWidth > 0.0f) {
                    const float inset = m_presentationBorderWidth;
                    const float innerW = std::max(0.0f, rect.width - inset * 2.0f);
                    const float innerH = std::max(0.0f, rect.height - inset * 2.0f);
                    if (innerW > 0.0f && innerH > 0.0f) {
                        canvas.drawRect({rect.x + inset, rect.y + inset, innerW, innerH}, m_presentationBackgroundColor);
                    }
                } else {
                    canvas.drawRect(rect, m_presentationBackgroundColor);
                }
            }
        } else if (m_topOnlyBorderRadius && hasBackground && !hasBorder) {
            canvas.drawTopRoundedRect(rect, m_presentationBorderRadius, m_presentationBackgroundColor, m_borderRoundness);
        } else {
            canvas.drawRoundedRect(rect, m_presentationBorderRadius, m_presentationBackgroundColor, m_presentationBorderColor,
                                   hasBorder ? m_presentationBorderWidth : 0.0f, m_borderRoundness);
        }
    }

    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
