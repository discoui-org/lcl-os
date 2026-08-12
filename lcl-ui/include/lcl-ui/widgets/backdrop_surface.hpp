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
    void addFilter(lcl::protocol::FilterType type, float value);
    void setGlass(float thicknessPx, float refractionFactor, float dispersionGain);
    void clearFilters();

    void setBlendMode(EffectBlend mode) { m_blendMode = mode; markDirty(); }
    EffectBlend getBlendMode() const { return m_blendMode; }

    void setOpacity(float opacity);
    float getOpacity() const { return m_opacity; }

    /** Keep backdrop filtering while opting out of button-like pointer visuals. */
    void setInteractive(bool interactive);
    bool isInteractive() const { return m_interactive; }
    void setOnClick(std::function<void()> callback) { m_onClick = std::move(callback); }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void collectEffects(std::vector<EffectRegion>& outEffects) const override;

private:
    void applyHoverVisuals();
    void applyPressedVisuals();
    void restoreBaseVisuals();

    std::vector<lcl::protocol::FilterOp> m_filters;
    EffectBlend m_blendMode{EffectBlend::Normal};
    float m_opacity{1.0f};
    std::function<void()> m_onClick{nullptr};
    bool m_pressed{false};
    bool m_hovered{false};
    bool m_focused{false};
    bool m_hasBaseVisuals{false};
    Color m_baseBackground{0, 0, 0, 0};
    Color m_baseBorder{0, 0, 0, 0};
    bool m_interactive{true};
};

} // namespace lcl::ui
