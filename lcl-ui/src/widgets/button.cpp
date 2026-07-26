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
        m_state = newState;
        markDirty();
    }
}

bool Button::onPointerMove(float px, float py) {
    bool inside = m_absoluteBounds.containsPoint(px, py);
    if (inside) {
        if (m_state != ButtonState::Active) {
            setState(ButtonState::Hover);
        }
        return true;
    } else {
        setState(ButtonState::Normal);
        return false;
    }
}

bool Button::onPointerDown(float px, float py) {
    if (m_absoluteBounds.containsPoint(px, py)) {
        setState(ButtonState::Active);
        return true;
    }
    return false;
}

bool Button::onPointerUp(float px, float py) {
    if (m_state == ButtonState::Active && m_absoluteBounds.containsPoint(px, py)) {
        setState(ButtonState::Hover);
        if (m_onClick) {
            m_onClick();
        }
        return true;
    }
    if (m_absoluteBounds.containsPoint(px, py)) {
        setState(ButtonState::Hover);
    } else {
        setState(ButtonState::Normal);
    }
    return false;
}

void Button::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;
    Container::draw(canvas, damageRect);
}

} // namespace lcl::ui
