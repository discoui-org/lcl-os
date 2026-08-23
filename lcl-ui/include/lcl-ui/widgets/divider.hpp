#pragma once

#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

/** A one-dimensional separator that follows its parent stack direction. */
class Divider final : public Widget {
public:
    Divider();
    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;
};

} // namespace lcl::ui
