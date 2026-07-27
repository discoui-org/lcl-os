#include "lcl-ui/widgets/button.hpp"
#include "render/skia_renderer.hpp"

namespace lcl::ui {

Button::Button(const std::string& label) {
    auto text = std::make_unique<Text>(label);
    m_textWidget = text.get();
    addChild(std::move(text));

    m_yogaNode.setPadding(YGEdgeHorizontal, 12.0f);
    m_yogaNode.setPadding(YGEdgeVertical, 8.0f);
    m_yogaNode.setJustifyContent(YGJustifyCenter);
    m_yogaNode.setAlignItems(YGAlignCenter);

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
        m_state = newState;
        markDirty();
    }
}

bool Button::onPointerEnter(const PointerEvent& event) {
    (void)event;
    if (m_state != ButtonState::Active) {
        setState(ButtonState::Hover);
    }
    return true;
}

bool Button::onPointerLeave(const PointerEvent& event) {
    (void)event;
    setState(ButtonState::Normal);
    return true;
}

bool Button::onPointerDown(const PointerEvent& event) {
    (void)event;
    setState(ButtonState::Active);
    return true;
}

bool Button::onPointerUp(const PointerEvent& event) {
    (void)event;
    if (m_state == ButtonState::Active) {
        setState(ButtonState::Hover);
        if (m_onClick) {
            m_onClick();
        }
        return true;
    }
    setState(ButtonState::Hover);
    return true;
}

void Button::draw(SkCanvas* canvas, const Rect& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    auto* renderer = reinterpret_cast<::lcl::render::SkiaRenderer*>(canvas);
    if (renderer) {
        ::lcl::render::SkiaRect r{m_absoluteBounds.x, m_absoluteBounds.y, m_absoluteBounds.width, m_absoluteBounds.height};
        ::lcl::render::SkiaColor btnBg;
        if (m_state == ButtonState::Hover) {
            btnBg = {59, 130, 246, 255}; // Bright Blue `#3B82F6`
        } else if (m_state == ButtonState::Active) {
            btnBg = {29, 78, 216, 255};  // Dark Blue `#1D4ED8`
        } else {
            btnBg = {37, 99, 235, 255};  // Primary Blue `#2563EB`
        }
        ::lcl::render::SkiaColor borderClr{147, 197, 253, 200}; // Light blue border `#93C5FD`
        renderer->drawRoundedRect(r, 8.0f, btnBg, borderClr, 1.5f);
    }

    Widget::draw(canvas, damageRect);
}

} // namespace lcl::ui
