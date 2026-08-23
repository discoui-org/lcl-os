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

    theme.typography.title = {theme.colors.primaryLabel, 18.0f};
    theme.typography.body = {theme.colors.primaryLabel, 14.0f};
    theme.typography.description = {theme.colors.secondaryLabel, 13.0f};
    theme.typography.caption = {theme.colors.tertiaryLabel, 12.0f};

    theme.text.normal.foreground = theme.colors.primaryLabel;

    theme.primarySurface.normal.background = theme.colors.primarySurface;
    theme.primarySurface.normal.border = theme.colors.separator;
    theme.primarySurface.normal.borderWidth = theme.metrics.separatorWidth;
    theme.primarySurface.normal.cornerRadius = theme.metrics.cardCornerRadius;

    theme.secondarySurface.normal.background = theme.colors.secondarySurface;
    theme.secondarySurface.normal.border = theme.colors.separator;
    theme.secondarySurface.normal.borderWidth = theme.metrics.separatorWidth;
    theme.secondarySurface.normal.cornerRadius = theme.metrics.controlCornerRadius;

    theme.groupedSurface.normal.background = theme.colors.groupedSurface;
    theme.groupedSurface.normal.border = theme.colors.separator;
    theme.groupedSurface.normal.borderWidth = theme.metrics.separatorWidth;
    theme.groupedSurface.normal.cornerRadius = theme.metrics.groupCornerRadius;

    theme.primaryButton.normal.background = theme.colors.accent;
    theme.primaryButton.normal.foreground = theme.colors.primaryLabel;
    theme.primaryButton.normal.border = graphics::Color{255, 255, 255, 46};
    theme.primaryButton.normal.borderWidth = theme.metrics.separatorWidth;
    theme.primaryButton.normal.cornerRadius = theme.metrics.controlCornerRadius;
    theme.primaryButton.normal.scale = 1.0f;
    theme.primaryButton.normal.opacity = 1.0f;
    theme.primaryButton.hover.background = theme.colors.accentHover;
    theme.primaryButton.hover.scale = 1.015f;
    theme.primaryButton.pressed.background = theme.colors.accentPressed;
    theme.primaryButton.pressed.scale = 0.965f;
    theme.primaryButton.pressed.opacity = 0.96f;
    theme.primaryButton.focused.border = theme.colors.focusRing;
    theme.primaryButton.focused.borderWidth = 2.0f;
    theme.primaryButton.disabled.background = theme.colors.disabledFill;
    theme.primaryButton.disabled.foreground = theme.colors.tertiaryLabel;
    theme.primaryButton.disabled.border = graphics::Color{255, 255, 255, 24};
    theme.primaryButton.disabled.scale = 1.0f;
    theme.primaryButton.disabled.opacity = 1.0f;
    theme.primaryButton.horizontalPadding = theme.metrics.controlHorizontalPadding;
    theme.primaryButton.verticalPadding = theme.metrics.controlVerticalPadding;

    theme.quietButton = theme.primaryButton;
    theme.quietButton.normal.background = theme.colors.controlFill;
    theme.quietButton.normal.border = theme.colors.separator;
    theme.quietButton.hover.background = theme.colors.controlFillHover;
    theme.quietButton.pressed.background = theme.colors.controlFillPressed;

    theme.textField.normal.background = theme.colors.secondarySurface;
    theme.textField.normal.foreground = theme.colors.primaryLabel;
    theme.textField.normal.secondaryForeground = theme.colors.tertiaryLabel;
    theme.textField.normal.accent = theme.colors.accent;
    theme.textField.normal.border = theme.colors.separator;
    theme.textField.normal.borderWidth = theme.metrics.separatorWidth;
    theme.textField.normal.cornerRadius = theme.metrics.controlCornerRadius;
    theme.textField.normal.scale = 1.0f;
    theme.textField.normal.opacity = 1.0f;
    theme.textField.focused.border = theme.colors.focusRing;
    theme.textField.focused.borderWidth = 2.0f;
    theme.textField.horizontalPadding = theme.metrics.controlHorizontalPadding;

    theme.toggle.normal.background = theme.colors.controlFill;
    theme.toggle.normal.foreground = theme.colors.primaryLabel;
    theme.toggle.normal.accent = theme.colors.accent;
    theme.toggle.normal.border = theme.colors.separator;
    theme.toggle.normal.borderWidth = theme.metrics.separatorWidth;
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
    theme.menuItem.normal.cornerRadius = theme.metrics.compactCornerRadius;
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
    theme.popover.normal.borderWidth = theme.metrics.separatorWidth;
    theme.popover.normal.cornerRadius = theme.metrics.controlCornerRadius;
    theme.popover.normal.scale = 1.0f;
    theme.popover.normal.opacity = 1.0f;
    theme.popover.horizontalPadding = theme.metrics.controlHorizontalPadding;
    theme.popover.verticalPadding = theme.metrics.controlHorizontalPadding;
    return theme;
}

const Theme& defaultTheme() {
    static const Theme theme = createDefaultTheme();
    return theme;
}

const TypographyStyle& typographyForRole(const Theme& theme, TextRole role) {
    switch (role) {
        case TextRole::Title: return theme.typography.title;
        case TextRole::Description: return theme.typography.description;
        case TextRole::Caption: return theme.typography.caption;
        case TextRole::Body:
        default: return theme.typography.body;
    }
}

const WidgetStyle& widgetStyleForRole(const Theme& theme,
                                      WidgetStyleRole role) {
    switch (role) {
        case WidgetStyleRole::PrimarySurface: return theme.primarySurface;
        case WidgetStyleRole::SecondarySurface: return theme.secondarySurface;
        case WidgetStyleRole::GroupedSurface: return theme.groupedSurface;
        case WidgetStyleRole::QuietButton: return theme.quietButton;
        case WidgetStyleRole::TextField: return theme.textField;
        case WidgetStyleRole::Toggle: return theme.toggle;
        case WidgetStyleRole::MenuItem: return theme.menuItem;
        case WidgetStyleRole::Popover: return theme.popover;
        case WidgetStyleRole::PrimaryButton:
        default: return theme.primaryButton;
    }
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
