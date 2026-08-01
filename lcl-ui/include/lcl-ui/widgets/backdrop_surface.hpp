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

    void setOnClick(std::function<void()> callback) { m_onClick = std::move(callback); }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;

    void collectEffects(std::vector<EffectRegion>& outEffects) const override;

private:
    std::vector<lcl::protocol::FilterOp> m_filters;
    EffectBlend m_blendMode{EffectBlend::Normal};
    float m_opacity{1.0f};
    std::function<void()> m_onClick{nullptr};
    bool m_pressed{false};
};

} // namespace lcl::ui
