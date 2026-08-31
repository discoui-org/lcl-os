#pragma once

#include <algorithm>
#include <cmath>

namespace lcl::ui {

/** Logical inset values supplied by the platform/windowing layer. */
struct LayoutInsets {
    float top{0.0f};
    float right{0.0f};
    float bottom{0.0f};
    float left{0.0f};

    constexpr bool operator==(const LayoutInsets&) const = default;
};

/** Minimum simultaneous widths used to select the adaptive size class. */
struct LayoutSizeClassPolicy {
    float minimumSidebarWidth{280.0f};
    float minimumDetailWidth{480.0f};

    constexpr bool operator==(const LayoutSizeClassPolicy&) const = default;
};

enum class LayoutSizeClass {
    Compact,
    Expanded,
};

/**
 * Backend-neutral logical window geometry for adaptive application layouts.
 *
 * `width` and `height` are the client surface extent. `availableWidth` and
 * `availableHeight` exclude the supplied safe-area insets. The size class is
 * derived exclusively from the available width and the declared minimum
 * sidebar/detail widths; it never depends on platform or device names.
 */
struct LayoutEnvironment {
    float width{0.0f};
    float height{0.0f};
    LayoutInsets safeArea{};
    float availableWidth{0.0f};
    float availableHeight{0.0f};
    LayoutSizeClass sizeClass{LayoutSizeClass::Compact};

    constexpr bool operator==(const LayoutEnvironment&) const = default;
    constexpr bool operator!=(const LayoutEnvironment& other) const {
        return !(*this == other);
    }
};

inline float sanitizeLayoutInset(float value) noexcept {
    return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
}

inline LayoutInsets sanitizeLayoutInsets(LayoutInsets value) noexcept {
    value.top = sanitizeLayoutInset(value.top);
    value.right = sanitizeLayoutInset(value.right);
    value.bottom = sanitizeLayoutInset(value.bottom);
    value.left = sanitizeLayoutInset(value.left);
    return value;
}

inline LayoutSizeClassPolicy sanitizeLayoutSizeClassPolicy(
        LayoutSizeClassPolicy value) noexcept {
    value.minimumSidebarWidth = sanitizeLayoutInset(value.minimumSidebarWidth);
    value.minimumDetailWidth = sanitizeLayoutInset(value.minimumDetailWidth);
    return value;
}

inline LayoutEnvironment makeLayoutEnvironment(
        float width, float height, LayoutInsets safeArea,
        LayoutSizeClassPolicy policy = {}) noexcept {
    LayoutEnvironment result{};
    result.width = std::isfinite(width) ? std::max(0.0f, width) : 0.0f;
    result.height = std::isfinite(height) ? std::max(0.0f, height) : 0.0f;
    result.safeArea = sanitizeLayoutInsets(safeArea);
    const LayoutSizeClassPolicy sanitizedPolicy = sanitizeLayoutSizeClassPolicy(policy);
    result.availableWidth = std::max(
        0.0f, result.width - result.safeArea.left - result.safeArea.right);
    result.availableHeight = std::max(
        0.0f, result.height - result.safeArea.top - result.safeArea.bottom);
    result.sizeClass = result.availableWidth >=
            sanitizedPolicy.minimumSidebarWidth + sanitizedPolicy.minimumDetailWidth
        ? LayoutSizeClass::Expanded
        : LayoutSizeClass::Compact;
    return result;
}

} // namespace lcl::ui
