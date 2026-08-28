#pragma once

#include "lcl-ui/widgets/container.hpp"

namespace lcl::ui {

/**
 * Container-based keyboard focus traversal boundary.
 *
 * FocusScope uses Container's normal layout/render behavior and owns no
 * focus itself. EventDispatcher treats the nearest ancestor scope of the
 * focused widget as the active, wrapping traversal context.
 */
class FocusScope final : public Container {
public:
    FocusScope() = default;
    ~FocusScope() override = default;

    bool isFocusScope() const noexcept override { return true; }
};

} // namespace lcl::ui
