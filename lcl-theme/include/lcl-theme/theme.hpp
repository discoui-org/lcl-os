#pragma once

#include "lcl-graphics/paint.hpp"

#include <optional>

namespace lcl::theme {

enum class StyleState {
    Normal,
    Hover,
    Pressed,
    Focused,
    Disabled,
};

enum class TextRole {
    Title,
    Body,
    Description,
    Caption,
};

enum class WidgetStyleRole {
    PrimarySurface,
    SecondarySurface,
    GroupedSurface,
    PrimaryButton,
    QuietButton,
    TextField,
    Toggle,
    Picker,
    Slider,
    ProgressView,
    TabView,
    MenuItem,
    Popover,
};

struct StyleValues {
    std::optional<graphics::Color> background;
    std::optional<graphics::Color> foreground;
    std::optional<graphics::Color> secondaryForeground;
    std::optional<graphics::Color> accent;
    std::optional<graphics::Color> border;
    std::optional<float> borderWidth;
    std::optional<float> cornerRadius;
    std::optional<float> scale;
    std::optional<float> opacity;
};

struct WidgetStyle {
    StyleValues normal;
    StyleValues hover;
    StyleValues pressed;
    StyleValues focused;
    StyleValues disabled;
    std::optional<float> horizontalPadding;
    std::optional<float> verticalPadding;
};

struct ResolvedStyle {
    graphics::Color background{0, 0, 0, 0};
    graphics::Color foreground{255, 255, 255, 255};
    graphics::Color secondaryForeground{255, 255, 255, 153};
    graphics::Color accent{10, 132, 255, 255};
    graphics::Color border{0, 0, 0, 0};
    float borderWidth{0.0f};
    float cornerRadius{0.0f};
    float scale{1.0f};
    float opacity{1.0f};
};

struct SemanticColors {
    graphics::Color desktop{0, 0, 0, 255};
    graphics::Color primarySurface{28, 28, 30, 255};
    graphics::Color secondarySurface{44, 44, 46, 255};
    graphics::Color groupedSurface{18, 18, 20, 255};
    graphics::Color elevatedSurface{44, 44, 46, 242};
    graphics::Color materialTint{20, 20, 24, 128};
    graphics::Color separator{84, 84, 88, 128};
    graphics::Color primaryLabel{255, 255, 255, 255};
    graphics::Color secondaryLabel{235, 235, 245, 153};
    graphics::Color tertiaryLabel{235, 235, 245, 76};
    graphics::Color accent{10, 132, 255, 255};
    graphics::Color accentHover{35, 145, 255, 255};
    graphics::Color accentPressed{0, 105, 220, 255};
    graphics::Color focusRing{100, 210, 255, 220};
    graphics::Color controlFill{120, 120, 128, 82};
    graphics::Color controlFillHover{132, 132, 140, 96};
    graphics::Color controlFillPressed{99, 99, 102, 110};
    graphics::Color controlShadow{0, 0, 0, 68};
    graphics::Color selectionFill{84, 84, 88, 92};
    graphics::Color selectionFillPressed{99, 99, 102, 128};
    graphics::Color disabledFill{72, 72, 74, 110};
    graphics::Color windowTitleFocused{10, 132, 255, 255};
    graphics::Color windowTitleBlurred{72, 72, 74, 255};
};

struct TypographyStyle {
    graphics::Color foreground{255, 255, 255, 255};
    float fontSize{14.0f};
};

struct Typography {
    TypographyStyle title;
    TypographyStyle body;
    TypographyStyle description;
    TypographyStyle caption;
};

struct ThemeMetrics {
    float compactControlHeight{32.0f};
    float regularControlHeight{38.0f};
    float largeControlHeight{44.0f};
    float controlHorizontalPadding{14.0f};
    float controlVerticalPadding{8.0f};
    float compactCornerRadius{6.0f};
    float controlCornerRadius{10.0f};
    float groupCornerRadius{12.0f};
    float cardCornerRadius{16.0f};
    float separatorWidth{1.0f};
    float checkboxSize{18.0f};
    float radioSize{18.0f};
    float sliderTrackHeight{4.0f};
    float sliderThumbSize{20.0f};
    float progressTrackHeight{4.0f};
    float tabBarHeight{54.0f};
    float groupPadding{8.0f};
    float cardPadding{20.0f};
};

struct Theme {
    SemanticColors colors;
    Typography typography;
    ThemeMetrics metrics;
    WidgetStyle text;
    WidgetStyle primarySurface;
    WidgetStyle secondarySurface;
    WidgetStyle groupedSurface;
    WidgetStyle primaryButton;
    WidgetStyle quietButton;
    WidgetStyle textField;
    WidgetStyle toggle;
    WidgetStyle picker;
    WidgetStyle slider;
    WidgetStyle progressView;
    WidgetStyle tabView;
    WidgetStyle menuItem;
    WidgetStyle popover;
};

Theme createDefaultTheme();
const Theme& defaultTheme();
const TypographyStyle& typographyForRole(const Theme& theme, TextRole role);
const WidgetStyle& widgetStyleForRole(const Theme& theme,
                                      WidgetStyleRole role);
ResolvedStyle resolveStyle(const WidgetStyle& style, StyleState state);

class ThemeContext {
public:
    ThemeContext();
    explicit ThemeContext(Theme theme);

    const Theme& value() const noexcept { return m_theme; }
    void setTheme(Theme theme);

private:
    Theme m_theme;
};

} // namespace lcl::theme
