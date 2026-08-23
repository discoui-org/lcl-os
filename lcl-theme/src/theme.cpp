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

    theme.text.normal.foreground = theme.colors.primaryLabel;

    theme.button.normal.background = theme.colors.accent;
    theme.button.normal.foreground = theme.colors.primaryLabel;
    theme.button.normal.border = graphics::Color{255, 255, 255, 46};
    theme.button.normal.borderWidth = 1.0f;
    theme.button.normal.cornerRadius = 10.0f;
    theme.button.normal.scale = 1.0f;
    theme.button.normal.opacity = 1.0f;
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

    theme.textField.normal.background = theme.colors.elevatedSurface;
    theme.textField.normal.foreground = theme.colors.primaryLabel;
    theme.textField.normal.secondaryForeground = theme.colors.tertiaryLabel;
    theme.textField.normal.accent = theme.colors.accent;
    theme.textField.normal.border = theme.colors.separator;
    theme.textField.normal.borderWidth = 1.0f;
    theme.textField.normal.cornerRadius = 8.0f;
    theme.textField.normal.scale = 1.0f;
    theme.textField.normal.opacity = 1.0f;
    theme.textField.focused.border = theme.colors.focusRing;
    theme.textField.focused.borderWidth = 2.0f;
    theme.textField.horizontalPadding = 10.0f;

    theme.toggle.normal.background = theme.colors.controlFill;
    theme.toggle.normal.foreground = theme.colors.primaryLabel;
    theme.toggle.normal.accent = theme.colors.accent;
    theme.toggle.normal.border = theme.colors.separator;
    theme.toggle.normal.borderWidth = 1.0f;
    theme.toggle.normal.scale = 1.0f;
    theme.toggle.normal.opacity = 1.0f;
    theme.toggle.hover.background = theme.colors.controlFillHover;
    theme.toggle.hover.accent = theme.colors.accentHover;
    theme.toggle.hover.scale = 1.015f;
    theme.toggle.pressed.background = theme.colors.controlFillPressed;
    theme.toggle.pressed.accent = theme.colors.accentPressed;
    theme.toggle.pressed.scale = 0.965f;
    theme.toggle.pressed.opacity = 0.96f;
    theme.toggle.focused.scale = 1.0f;
    theme.toggle.focused.opacity = 1.0f;
    theme.toggle.disabled.background = theme.colors.disabledFill;
    theme.toggle.disabled.scale = 1.0f;
    theme.toggle.disabled.opacity = 0.48f;

    theme.menuItem.normal.background = graphics::Color{0, 0, 0, 0};
    theme.menuItem.normal.foreground = theme.colors.primaryLabel;
    theme.menuItem.normal.border = graphics::Color{0, 0, 0, 0};
    theme.menuItem.normal.borderWidth = 0.0f;
    theme.menuItem.normal.cornerRadius = 6.0f;
    theme.menuItem.normal.scale = 1.0f;
    theme.menuItem.normal.opacity = 1.0f;
    theme.menuItem.hover.background = theme.colors.selectionFill;
    theme.menuItem.pressed.background = theme.colors.selectionFillPressed;
    theme.menuItem.focused.background = theme.colors.selectionFill;
    theme.menuItem.disabled.foreground = theme.colors.tertiaryLabel;
    theme.menuItem.disabled.opacity = 1.0f;
    theme.menuItem.horizontalPadding = 10.0f;
    theme.menuItem.verticalPadding = 6.0f;

    theme.popover.normal.background = theme.colors.elevatedSurface;
    theme.popover.normal.border = theme.colors.separator;
    theme.popover.normal.borderWidth = 1.0f;
    theme.popover.normal.cornerRadius = 10.0f;
    theme.popover.normal.scale = 1.0f;
    theme.popover.normal.opacity = 1.0f;
    theme.popover.horizontalPadding = 12.0f;
    theme.popover.verticalPadding = 12.0f;
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
        .secondaryForeground = resolveValue(
            selected.secondaryForeground, style.normal.secondaryForeground,
            graphics::Color{255, 255, 255, 153}),
        .accent = resolveValue(selected.accent, style.normal.accent,
                               graphics::Color{10, 132, 255, 255}),
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
