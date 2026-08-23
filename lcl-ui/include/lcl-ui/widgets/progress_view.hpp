#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <optional>

namespace lcl::ui {

enum class ProgressViewStyle {
    Automatic,
    Linear,
    Circular,
};

class ProgressView final : public Widget {
public:
    ProgressView();
    explicit ProgressView(float value, float total = 1.0f);
    ~ProgressView() override = default;

    void setValue(std::optional<float> value);
    std::optional<float> value() const noexcept { return m_value; }
    void setTotal(float total);
    float total() const noexcept { return m_total; }
    void setProgressViewStyle(ProgressViewStyle style);
    ProgressViewStyle progressViewStyle() const noexcept { return m_style; }

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

private:
    ProgressViewStyle resolvedProgressViewStyle() const noexcept;
    float normalizedValue() const noexcept;
    void updatePresentationRegistration();
    void tickIndeterminate(float deltaSec);
    void drawLinear(graphics::Canvas& canvas,
                    const lcl::theme::ResolvedStyle& visual);
    void drawCircular(graphics::Canvas& canvas,
                      const lcl::theme::ResolvedStyle& visual);
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    std::optional<float> m_value;
    float m_total{1.0f};
    float m_phase{0.0f};
    ProgressViewStyle m_style{ProgressViewStyle::Automatic};
};

} // namespace lcl::ui
