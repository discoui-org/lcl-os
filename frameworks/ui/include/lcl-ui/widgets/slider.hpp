#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <functional>

namespace lcl::ui {

class Slider final : public Widget {
public:
    using ChangeCallback = std::function<void(float)>;
    using EditingCallback = std::function<void(bool)>;

    explicit Slider(float value = 0.0f, float minimum = 0.0f,
                    float maximum = 1.0f, float step = 0.0f);

    void setValue(float value);
    float value() const noexcept { return m_value; }
    void setRange(float minimum, float maximum);
    float minimum() const noexcept { return m_minimum; }
    float maximum() const noexcept { return m_maximum; }
    void setStep(float step);
    float step() const noexcept { return m_step; }
    void setOnChange(ChangeCallback callback) { m_onChange = std::move(callback); }
    void setOnEditingChanged(EditingCallback callback) {
        m_onEditingChanged = std::move(callback);
    }
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled; }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onKeyDown(const KeyEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

private:
    float normalizedValue() const noexcept;
    float normalizedAndStepped(float value) const noexcept;
    void updateFromPointer(float x);
    void setEditing(bool editing);
    lcl::theme::StyleState visualStyleState() const noexcept;
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    float m_value{0.0f};
    float m_minimum{0.0f};
    float m_maximum{1.0f};
    float m_step{0.0f};
    bool m_enabled{true};
    bool m_hovered{false};
    bool m_pressed{false};
    bool m_focused{false};
    bool m_dragging{false};
    ChangeCallback m_onChange;
    EditingCallback m_onEditingChanged;
};

} // namespace lcl::ui
