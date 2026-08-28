#include "lcl-ui/widgets/button.hpp"

namespace lcl::ui {

Button::Button(const std::string& label) {
    auto text = std::make_unique<Text>(label);
    m_textWidget = text.get();
    addChild(std::move(text));

    setJustifyContent(layout::Justify::Center);
    setAlignItems(layout::Align::Center);
    setFocusable(true);
    styleDidChange();
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
    const auto* style = resolvedStyle();
    if (!style) return;

    lcl::theme::StyleState styleState = lcl::theme::StyleState::Normal;
    if (m_state == ButtonState::Hover) styleState = lcl::theme::StyleState::Hover;
    else if (m_state == ButtonState::Active) styleState = lcl::theme::StyleState::Pressed;
    else if (m_state == ButtonState::Focused) styleState = lcl::theme::StyleState::Focused;
    else if (m_state == ButtonState::Disabled) styleState = lcl::theme::StyleState::Disabled;
    const auto visual = lcl::theme::resolveStyle(*style, styleState);

    const auto& theme = interactionMotionTheme();
    const lcl::motion::Motion* scaleMotion = &theme.hover;
    if (m_state == ButtonState::Active) scaleMotion = &theme.pressed;
    if (previous == ButtonState::Active && m_state != ButtonState::Active) scaleMotion = &theme.release;

    setBorderColor(visual.border);
    setBorderWidth(visual.borderWidth);
    setBorderRadius(visual.cornerRadius);
    if (m_textWidget) m_textWidget->setTextColor(visual.foreground);

    if (!theme.enabled || !m_motionCoordinator) {
        setScale(visual.scale);
        setOpacity(visual.opacity);
        setBackgroundColor(visual.background);
    } else {
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX,
            m_presentation.scaleX, visual.scale, *scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY,
            m_presentation.scaleY, visual.scale, *scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::Opacity,
            m_presentation.opacity, visual.opacity, *scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::Opacity, value); });
        animateBackgroundColor(visual.background, theme.focusTransition);
    }
    markDirty();
}

const lcl::theme::WidgetStyle* Button::defaultStyle() const noexcept {
    return &getTheme().primaryButton;
}

void Button::styleDidChange() {
    const auto* style = resolvedStyle();
    if (!style) return;
    if (!m_hasHeight) {
        setDefaultHeight(getTheme().metrics.regularControlHeight);
    }
    setPadding(layout::Edge::Horizontal, style->horizontalPadding.value_or(0.0f));
    setPadding(layout::Edge::Vertical, style->verticalPadding.value_or(0.0f));
    applyStateMotion(m_state);
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

void Button::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    Container::draw(canvas, damageRect);
}

} // namespace lcl::ui
