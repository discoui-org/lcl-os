#pragma once

#include "lcl-ui/widgets/measured_widget.hpp"

#include <optional>

namespace lcl::ui {

/** A one-dimensional separator that follows its parent stack direction. */
class Divider final : public MeasuredWidget {
public:
    void setColor(graphics::Color color) {
        if (m_color && m_color->toARGB() == color.toARGB()) return;
        m_color = color;
        invalidatePaint();
    }
    void clearColor() {
        if (!m_color) return;
        m_color.reset();
        invalidatePaint();
    }

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

protected:
    layout::Size measure(const layout::Constraints& constraints) override;

private:
    std::optional<graphics::Color> m_color;
};

} // namespace lcl::ui
