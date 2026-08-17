#include "lcl-ui/widgets/button.hpp"

namespace lcl::ui {

Button::Button(const std::string& label) {
    auto text = std::make_unique<Text>(label);
    m_textWidget = text.get();
    addChild(std::move(text));

    m_yogaNode.setPadding(YGEdgeHorizontal, 12.0f);
    m_yogaNode.setPadding(YGEdgeVertical, 8.0f);
    m_yogaNode.setJustifyContent(YGJustifyCenter);
    m_yogaNode.setAlignItems(YGAlignCenter);
    setBorderRadius(8.0f);
    setBackgroundColor({37, 99, 235, 255});
    setBorderColor({147, 197, 253, 200});
    setBorderWidth(1.5f);

    setFocusable(true);
}

void Button::setLabel(const std::string& label) {
    if (m_textWidget) {
        m_textWidget->setText(label);
    }
}

std::string Button::getLabel() const {
    return m_textWidget ? m_textWidget->getText() : "";
}

void Button::setState(ButtonState newState) {
    if (m_state != newState) {
        const ButtonState previous = m_state;
        m_state = newState;
        applyStateMotion(previous);
    }
}

void Button::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    // Keep the generic input/focus eligibility contract authoritative so
    // EventDispatcher does not need Button-specific disabled logic.
    setInteractionEnabled(enabled);
    if (!enabled) { m_pressed = false; m_hovered = false; }
    updateComposedState();
}

void Button::updateComposedState() {
    if (!m_enabled) setState(ButtonState::Disabled);
    else if (m_pressed) setState(ButtonState::Active);
    else if (m_hovered) setState(ButtonState::Hover);
    else if (m_focused) setState(ButtonState::Focused);
    else setState(ButtonState::Normal);
}

void Button::applyStateMotion(ButtonState previous) {
    const auto& theme = interactionMotionTheme();
    float targetScale = 1.0f;
    Color targetColor{37, 99, 235, 255};
    const lcl::motion::Motion* scaleMotion = &theme.hover;
    if (m_state == ButtonState::Hover) { targetScale = theme.hoverScale; targetColor = {59, 130, 246, 255}; }
    else if (m_state == ButtonState::Active) { targetScale = theme.pressedScale; targetColor = {29, 78, 216, 255}; scaleMotion = &theme.pressed; }
    else if (m_state == ButtonState::Focused) { targetColor = {45, 110, 238, 255}; }
    else if (m_state == ButtonState::Disabled) { targetColor = {71, 85, 105, 160}; }
    if (previous == ButtonState::Active && m_state != ButtonState::Active) scaleMotion = &theme.release;

    if (!theme.enabled || !m_motionCoordinator) {
        setScale(targetScale);
        setBackgroundColor(targetColor);
    } else {
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX,
            m_presentation.scaleX, targetScale, *scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY,
            m_presentation.scaleY, targetScale, *scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
        animateBackgroundColor(targetColor, theme.focusTransition);
    }
    markDirty();
}

bool Button::onPointerEnter(const PointerEvent& event) {
    (void)event;
    if (!m_enabled) return false;
    m_hovered = true;
    updateComposedState();
    return true;
}

bool Button::onPointerLeave(const PointerEvent& event) {
    (void)event;
    if (!m_enabled) return false;
    m_hovered = false;
    m_pressed = false;
    updateComposedState();
    return true;
}

bool Button::onPointerDown(const PointerEvent& event) {
    (void)event;
    if (!m_enabled) return false;
    m_pressed = true;
    updateComposedState();
    return true;
}

bool Button::onPointerUp(const PointerEvent& event) {
    if (!m_enabled) return false;
    if (m_pressed) {
        m_pressed = false;
        m_hovered = (event.source == PointerSource::Mouse);
        updateComposedState();
        if (m_onClick) {
            m_onClick();
        }
        return true;
    }
    m_pressed = false;
    m_hovered = (event.source == PointerSource::Mouse);
    updateComposedState();
    return true;
}

bool Button::onPointerCancel(const PointerEvent& event) {
    (void)event;
    if (!m_enabled) return false;
    m_pressed = false;
    m_hovered = false;
    updateComposedState();
    return true;
}

bool Button::onFocusGained(const FocusEvent& event) {
    (void)event;
    m_focused = true;
    updateComposedState();
    return false;
}

bool Button::onFocusLost(const FocusEvent& event) {
    (void)event;
    m_focused = false;
    m_pressed = false;
    updateComposedState();
    return false;
}

void Button::draw(Canvas& canvas, const Rect& damageRect) {
    Container::draw(canvas, damageRect);
}

} // namespace lcl::ui
