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

struct StyleValues {
    std::optional<graphics::Color> background;
    std::optional<graphics::Color> foreground;
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
    graphics::Color border{0, 0, 0, 0};
    float borderWidth{0.0f};
    float cornerRadius{0.0f};
    float scale{1.0f};
    float opacity{1.0f};
};

struct SemanticColors {
    graphics::Color desktop{0, 0, 0, 255};
    graphics::Color surface{28, 28, 30, 255};
    graphics::Color elevatedSurface{44, 44, 46, 242};
    graphics::Color separator{84, 84, 88, 128};
    graphics::Color primaryLabel{255, 255, 255, 255};
    graphics::Color secondaryLabel{235, 235, 245, 153};
    graphics::Color accent{10, 132, 255, 255};
    graphics::Color accentHover{35, 145, 255, 255};
    graphics::Color accentPressed{0, 105, 220, 255};
    graphics::Color focusRing{100, 210, 255, 220};
    graphics::Color disabledFill{72, 72, 74, 110};
    graphics::Color windowTitleFocused{10, 132, 255, 255};
    graphics::Color windowTitleBlurred{72, 72, 74, 255};
};

struct Theme {
    SemanticColors colors;
    WidgetStyle button;
};

Theme createDefaultTheme();
const Theme& defaultTheme();
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
