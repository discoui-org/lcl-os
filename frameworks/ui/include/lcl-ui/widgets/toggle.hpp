#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <functional>
#include <string>
#include <utility>

namespace lcl::ui {

enum class ToggleStyle {
    Automatic,
    Switch,
    Checkbox,
    Button,
};

/**
 * Compact boolean control with a practical default hit target.
 *
 * Value is model state and changes synchronously. The thumb position is a
 * presentation value animated by the owning WindowApp's MotionCoordinator.
 */
class Toggle final : public Widget {
public:
    explicit Toggle(bool value = false);
    explicit Toggle(std::string label, bool value = false);
    explicit Toggle(const char* label, bool value = false)
        : Toggle(std::string(label ? label : ""), value) {}

    void setLabel(std::string label);
    const std::string& label() const noexcept { return m_label; }
    void setToggleStyle(ToggleStyle style);
    ToggleStyle toggleStyle() const noexcept { return m_style; }

    void setValue(bool value);
    bool value() const noexcept { return m_value; }
    void setMixed(bool mixed);
    bool isMixed() const noexcept { return m_mixed; }
    void setOnChange(std::function<void(bool)> callback) {
        m_onChange = std::move(callback);
    }

    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled; }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onKeyDown(const KeyEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;

private:
    struct VisualColors {
        graphics::Color track;
        graphics::Color trackBorder;
        graphics::Color thumb;
        graphics::Color thumbBorder;
        float trackBorderWidth{0.0f};
    };

    graphics::RectF trackRect() const noexcept;
    graphics::RectF thumbRect() const noexcept;
    graphics::RectF checkboxRect() const noexcept;
    ToggleStyle resolvedToggleStyle() const noexcept;
    VisualColors visualColors() const noexcept;
    lcl::theme::StyleState visualStyleState() const noexcept;
    void setThumbPresentation(float progress);
    void retargetThumb();
    void toggleValue();
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    bool m_value{false};
    bool m_mixed{false};
    bool m_enabled{true};
    bool m_hovered{false};
    bool m_pressed{false};
    bool m_focused{false};
    bool m_pointerArmed{false};
    float m_thumbProgress{0.0f};
    std::string m_label;
    ToggleStyle m_style{ToggleStyle::Automatic};
    std::function<void(bool)> m_onChange;
};

} // namespace lcl::ui
