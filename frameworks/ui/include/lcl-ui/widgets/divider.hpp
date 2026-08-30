#pragma once

#include "lcl-ui/widgets/measured_widget.hpp"

namespace lcl::ui {

/** A one-dimensional separator that follows its parent stack direction. */
class Divider final : public MeasuredWidget {
public:
    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

protected:
    layout::Size measure(const layout::Constraints& constraints) override;
};

} // namespace lcl::ui
