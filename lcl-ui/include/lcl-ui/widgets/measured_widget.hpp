#pragma once

#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

/** Base class for leaf widgets whose content determines their intrinsic size. */
class MeasuredWidget : public Widget {
public:
    MeasuredWidget();
    ~MeasuredWidget() override = default;

protected:
    virtual layout::Size measure(const layout::Constraints& constraints) = 0;
    void invalidateMeasurement();
};

} // namespace lcl::ui
