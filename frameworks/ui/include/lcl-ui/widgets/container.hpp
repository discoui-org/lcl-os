#pragma once

#include <algorithm>

#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

class Container : public Widget {
public:
    Container();
    ~Container() override = default;

    void setBackgroundColor(const graphics::Color& color);
    void animateBackgroundColor(const graphics::Color& color, const lcl::motion::Motion& motion);
    graphics::Color getBackgroundColor() const { return m_backgroundColor; }
    graphics::Color getPresentationBackgroundColor() const { return m_presentationBackgroundColor; }

    void setBorderColor(const graphics::Color& color);
    void animateBorderColor(const graphics::Color& color, const lcl::motion::Motion& motion);
    graphics::Color getBorderColor() const { return m_borderColor; }

    void setBorderWidth(float width);
    float getBorderWidth() const { return m_borderWidth; }

    void setBorderRadius(float radius);
    float getBorderRadius() const { return m_borderRadius; }

    // Preserve only upper corner arcs while the lower edge stays flush.
    void setTopOnlyBorderRadius(bool enabled) {
        if (m_topOnlyBorderRadius == enabled) return;
        m_topOnlyBorderRadius = enabled;
        invalidatePaint();
    }
    bool hasTopOnlyBorderRadius() const { return m_topOnlyBorderRadius; }

    void setBorderRoundness(float roundness) {
        const float next = std::clamp(roundness, 2.0f, 8.0f);
        if (m_borderRoundness == next) return;
        m_borderRoundness = next;
        invalidatePaint();
    }
    float getBorderRoundness() const { return m_borderRoundness; }

    float getPresentationValue(AnimatableProperty property) const override;
    void applyPresentationValue(AnimatableProperty property, float value) override;
    void commitModelValue(AnimatableProperty property, float value) override;

    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;

protected:
    graphics::RectF getUntransformedPaintBounds() const noexcept override;
    void styleDidChange() override;

private:
    graphics::Color m_backgroundColor{0, 0, 0, 0};
    graphics::Color m_presentationBackgroundColor{0, 0, 0, 0};
    graphics::Color m_borderColor{0, 0, 0, 0};
    graphics::Color m_presentationBorderColor{0, 0, 0, 0};
    float m_borderWidth{0.0f};
    float m_presentationBorderWidth{0.0f};
    float m_borderRadius{0.0f};
    float m_presentationBorderRadius{0.0f};
    float m_borderRoundness{3.2f};
    bool m_topOnlyBorderRadius{false};
};

} // namespace lcl::ui
