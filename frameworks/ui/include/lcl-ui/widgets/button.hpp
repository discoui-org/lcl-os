#pragma once

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include <functional>
#include <string>

namespace lcl::ui {

enum class ButtonState {
    Normal,
    Focused,
    Hover,
    Active,
    Disabled,
};

class Button : public Container {
public:
    explicit Button(const std::string& label = "");
    ~Button() override = default;

    void setLabel(const std::string& label);
    std::string getLabel() const;

    Text* getTextWidget() const { return m_textWidget; }

    void setOnClick(std::function<void()> callback) override { m_onClick = std::move(callback); }

    ButtonState getState() const { return m_state; }
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled; }

    // Phase 1.5 Event Handlers
    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;

private:
    void setState(ButtonState newState);
    void updateComposedState();
    void applyStateMotion(ButtonState previous);
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    ButtonState m_state{ButtonState::Normal};
    Text* m_textWidget{nullptr};
    std::function<void()> m_onClick{nullptr};
    bool m_hovered{false};
    bool m_pressed{false};
    bool m_focused{false};
    bool m_enabled{true};
};

} // namespace lcl::ui
