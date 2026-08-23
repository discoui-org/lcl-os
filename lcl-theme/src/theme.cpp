#include "lcl-theme/theme.hpp"

#include <algorithm>
#include <utility>

namespace lcl::theme {
namespace {

const StyleValues& valuesForState(const WidgetStyle& style, StyleState state) {
    switch (state) {
        case StyleState::Hover: return style.hover;
        case StyleState::Pressed: return style.pressed;
        case StyleState::Focused: return style.focused;
        case StyleState::Disabled: return style.disabled;
        case StyleState::Normal:
        default: return style.normal;
    }
}

template <typename T>
T resolveValue(const std::optional<T>& selected,
               const std::optional<T>& normal,
               T fallback) {
    return selected.value_or(normal.value_or(std::move(fallback)));
}

} // namespace

Theme createDefaultTheme() {
    Theme theme;

    theme.button.normal = {
        .background = theme.colors.accent,
        .foreground = theme.colors.primaryLabel,
        .border = graphics::Color{255, 255, 255, 46},
        .borderWidth = 1.0f,
        .cornerRadius = 10.0f,
        .scale = 1.0f,
        .opacity = 1.0f,
    };
    theme.button.hover.background = theme.colors.accentHover;
    theme.button.hover.scale = 1.015f;
    theme.button.pressed.background = theme.colors.accentPressed;
    theme.button.pressed.scale = 0.965f;
    theme.button.pressed.opacity = 0.96f;
    theme.button.focused.border = theme.colors.focusRing;
    theme.button.focused.borderWidth = 2.0f;
    theme.button.disabled.background = theme.colors.disabledFill;
    theme.button.disabled.foreground = graphics::Color{235, 235, 245, 120};
    theme.button.disabled.border = graphics::Color{255, 255, 255, 24};
    theme.button.disabled.scale = 1.0f;
    theme.button.disabled.opacity = 1.0f;
    theme.button.horizontalPadding = 14.0f;
    theme.button.verticalPadding = 8.0f;
    return theme;
}

const Theme& defaultTheme() {
    static const Theme theme = createDefaultTheme();
    return theme;
}

ResolvedStyle resolveStyle(const WidgetStyle& style, StyleState state) {
    const StyleValues& selected = valuesForState(style, state);
    return {
        .background = resolveValue(selected.background, style.normal.background,
                                   graphics::Color{0, 0, 0, 0}),
        .foreground = resolveValue(selected.foreground, style.normal.foreground,
                                   graphics::Color{255, 255, 255, 255}),
        .border = resolveValue(selected.border, style.normal.border,
                               graphics::Color{0, 0, 0, 0}),
        .borderWidth = std::max(0.0f, resolveValue(
            selected.borderWidth, style.normal.borderWidth, 0.0f)),
        .cornerRadius = std::max(0.0f, resolveValue(
            selected.cornerRadius, style.normal.cornerRadius, 0.0f)),
        .scale = std::max(0.0f, resolveValue(
            selected.scale, style.normal.scale, 1.0f)),
        .opacity = std::clamp(resolveValue(
            selected.opacity, style.normal.opacity, 1.0f), 0.0f, 1.0f),
    };
}

ThemeContext::ThemeContext()
    : m_theme(createDefaultTheme()) {}

ThemeContext::ThemeContext(Theme theme)
    : m_theme(std::move(theme)) {}

void ThemeContext::setTheme(Theme theme) {
    m_theme = std::move(theme);
}

} // namespace lcl::theme
