#pragma once

namespace lcl::ui::touch_interaction {

// Logical UI coordinates. The boundary is inclusive: a displacement at or
// beyond this value is no longer a tap and may become a scroll drag.
inline constexpr float kTouchSlop = 8.0f;

inline constexpr bool exceedsSlop(float deltaX, float deltaY) noexcept {
    return deltaX * deltaX + deltaY * deltaY >= kTouchSlop * kTouchSlop;
}

} // namespace lcl::ui::touch_interaction
