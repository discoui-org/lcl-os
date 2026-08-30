#include "lcl-ui/widgets/measured_widget.hpp"

namespace lcl::ui {

MeasuredWidget::MeasuredWidget() {
    setMeasureCallback([this](const layout::Constraints& constraints) {
        return measure(constraints);
    });
}

void MeasuredWidget::invalidateMeasurement() {
    Widget::invalidateMeasurement();
}

} // namespace lcl::ui
