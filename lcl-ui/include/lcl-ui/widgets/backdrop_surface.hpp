#pragma once

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/core/effects.hpp"
#include <functional>

namespace lcl::ui {

class BackdropSurface : public Container {
public:
    BackdropSurface();
    ~BackdropSurface() override = default;

    void setFilters(const std::vector<lcl::protocol::FilterOp>& filters);
    void addFilter(lcl::protocol::FilterType type, float value,
                   float parameter1 = 1.4f, float parameter2 = 7.0f);
    void addFilter(const lcl::protocol::FilterOp& filter);
    void clearFilters();
    /** Composite one tint over the filtered pixels in the compositor effect pass. */
    void setTint(const graphics::Color& color);
    graphics::Color getTint() const { return m_tint; }

    void setEffectBounds(EffectBounds bounds) {
        if (m_effectBounds != bounds) {
            m_effectBounds = bounds;
            markDirty();
        }
    }
    EffectBounds getEffectBounds() const { return m_effectBounds; }

    void setBlendMode(EffectBlend mode) { m_blendMode = mode; markDirty(); }
    EffectBlend getBlendMode() const { return m_blendMode; }

    void setOpacity(float opacity);
    float getOpacity() const { return m_opacity; }

    /** Keep backdrop filtering while opting out of button-like pointer visuals. */
    void setInteractive(bool interactive);
    bool isInteractive() const { return m_interactive; }
    void setOnClick(std::function<void()> callback) override { m_onClick = std::move(callback); }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void collectEffects(std::vector<EffectRegion>& outEffects) const override;

private:
    void applyHoverVisuals();
    void applyPressedVisuals();
    void restoreBaseVisuals();

    std::vector<lcl::protocol::FilterOp> m_filters;
    graphics::Color m_tint{0, 0, 0, 0};
    EffectBounds m_effectBounds{EffectBounds::Local};
    EffectBlend m_blendMode{EffectBlend::Normal};
    float m_opacity{1.0f};
    std::function<void()> m_onClick{nullptr};
    bool m_pressed{false};
    bool m_hovered{false};
    bool m_focused{false};
    bool m_hasBaseVisuals{false};
    graphics::Color m_baseBackground{0, 0, 0, 0};
    graphics::Color m_baseBorder{0, 0, 0, 0};
    bool m_interactive{true};
};

} // namespace lcl::ui
