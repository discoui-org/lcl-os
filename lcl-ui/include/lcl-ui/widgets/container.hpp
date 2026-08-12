#pragma once

#include <algorithm>

#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

class Container : public Widget {
public:
    Container();
    ~Container() override = default;

    void setBackgroundColor(const Color& color);
    void animateBackgroundColor(const Color& color, const lcl::motion::Motion& motion);
    Color getBackgroundColor() const { return m_backgroundColor; }
    Color getPresentationBackgroundColor() const { return m_presentationBackgroundColor; }

    void setBorderColor(const Color& color);
    void animateBorderColor(const Color& color, const lcl::motion::Motion& motion);
    Color getBorderColor() const { return m_borderColor; }

    void setBorderWidth(float width);
    float getBorderWidth() const { return m_borderWidth; }

    void setBorderRadius(float radius);
    float getBorderRadius() const { return m_borderRadius; }

    // Preserve only upper corner arcs while the lower edge stays flush.
    void setTopOnlyBorderRadius(bool enabled) { m_topOnlyBorderRadius = enabled; markDirty(); }
    bool hasTopOnlyBorderRadius() const { return m_topOnlyBorderRadius; }

    void setBorderRoundness(float roundness) {
        m_borderRoundness = std::clamp(roundness, 2.0f, 8.0f);
        markDirty();
    }
    float getBorderRoundness() const { return m_borderRoundness; }

    float getPresentationValue(AnimatableProperty property) const override;
    void applyPresentationValue(AnimatableProperty property, float value) override;
    void commitModelValue(AnimatableProperty property, float value) override;

    void draw(Canvas& canvas, const Rect& damageRect) override;

private:
    Color m_backgroundColor{0, 0, 0, 0};
    Color m_presentationBackgroundColor{0, 0, 0, 0};
    Color m_borderColor{0, 0, 0, 0};
    Color m_presentationBorderColor{0, 0, 0, 0};
    float m_borderWidth{0.0f};
    float m_presentationBorderWidth{0.0f};
    float m_borderRadius{0.0f};
    float m_presentationBorderRadius{0.0f};
    float m_borderRoundness{3.2f};
    bool m_topOnlyBorderRadius{false};
};

} // namespace lcl::ui
