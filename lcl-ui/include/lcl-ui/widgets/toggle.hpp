#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <functional>
#include <utility>

namespace lcl::ui {

/**
 * Compact boolean control with a practical default hit target.
 *
 * Value is model state and changes synchronously. The thumb position is a
 * presentation value animated by the owning WindowApp's MotionCoordinator.
 */
class Toggle final : public Widget {
public:
    explicit Toggle(bool value = false);

    void setValue(bool value);
    bool value() const noexcept { return m_value; }
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

    void draw(Canvas& canvas, const Rect& damageRect) override;

private:
    struct VisualColors {
        Color track;
        Color trackBorder;
        Color thumb;
        Color thumbBorder;
    };

    Rect trackRect() const noexcept;
    Rect thumbRect() const noexcept;
    VisualColors visualColors() const noexcept;
    void setThumbPresentation(float progress);
    void retargetThumb();

    bool m_value{false};
    bool m_enabled{true};
    bool m_hovered{false};
    bool m_pressed{false};
    bool m_focused{false};
    bool m_pointerArmed{false};
    float m_thumbProgress{0.0f};
    std::function<void(bool)> m_onChange;
};

} // namespace lcl::ui
