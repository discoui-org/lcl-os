#pragma once

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/core/effects.hpp"

namespace lcl::ui {

class FilterGroup : public Container {
public:
    FilterGroup();
    ~FilterGroup() override = default;

    void setFilters(const std::vector<lcl::protocol::FilterOp>& filters);
    void addFilter(lcl::protocol::FilterType type, float value);
    void clearFilters();

    void setBlendMode(EffectBlend mode) {
        if (m_blendMode == mode) return;
        m_blendMode = mode;
        invalidatePaint();
    }
    EffectBlend getBlendMode() const { return m_blendMode; }

    void setOpacity(float opacity);
    float getOpacity() const { return m_opacity; }

    void collectEffects(std::vector<EffectRegion>& outEffects) const override;

private:
    std::vector<lcl::protocol::FilterOp> m_filters;
    EffectBlend m_blendMode{EffectBlend::Normal};
    float m_opacity{1.0f};
};

} // namespace lcl::ui
