#include "lcl-ui/widgets/window_chrome.hpp"

#include <algorithm>
#include <utility>

namespace lcl::ui::chrome {

WindowChromeSurface::WindowChromeSurface(
        float width, float titleHeight, float cornerRadius, std::string title,
        float titleFontSize, WindowChromeStyle style,
        WindowChromeActions actions)
    : m_chrome(std::move(title), style), m_actions(std::move(actions)),
      m_layout(m_chrome.layout(width, titleHeight, cornerRadius,
                               titleFontSize)),
      m_titleHeight(titleHeight), m_cornerRadius(cornerRadius),
      m_titleFontSize(titleFontSize) {
    setPositionType(layout::PositionType::Absolute);
    setPosition(layout::Edge::Left, 0.0f);
    setPosition(layout::Edge::Top, 0.0f);
    setWidth(width);
    setHeight(titleHeight);
}

graphics::DisplayList WindowChromeSurface::buildChromeDisplayList(
        const graphics::RectF& bounds) const {
    return m_chrome.buildDisplayList({
        bounds,
        std::min(m_titleHeight, bounds.height),
        m_cornerRadius,
        m_titleFontSize,
        1.0f,
        true,
        m_chrome.style().titleBarBackground,
    });
}

void WindowChromeSurface::setFrameSize(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return;
    if (m_titleHeight == height &&
        getPresentationValue(AnimatableProperty::Width) == width &&
        getPresentationValue(AnimatableProperty::Height) == height) return;
    m_titleHeight = height;
    setWidth(width);
    setHeight(height);
}

void WindowChromeSurface::syncLayout(float parentAbsX, float parentAbsY) {
    Widget::syncLayout(parentAbsX, parentAbsY);
    m_layout = m_chrome.layout(
        m_absoluteBounds.width,
        std::min(m_titleHeight, m_absoluteBounds.height),
        m_cornerRadius, m_titleFontSize);
}

void WindowChromeSurface::draw(graphics::Canvas& canvas,
                               const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect)) return;
    beginPresentation(canvas, damageRect);
    canvas.drawDisplayList(buildChromeDisplayList(m_absoluteBounds));
    endPresentation(canvas);
}

int WindowChromeSurface::hitTest(const PointerEvent& event) const {
    return m_chrome.hitTest(
        event.x - m_absoluteBounds.x,
        event.y - m_absoluteBounds.y,
        m_absoluteBounds.width,
        std::min(m_titleHeight, m_absoluteBounds.height),
        m_cornerRadius);
}

bool WindowChromeSurface::onPointerEnter(const PointerEvent& event) {
    if (event.source != PointerSource::Mouse) return false;
    const bool changed = m_chrome.pointerMove(hitTest(event));
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::onPointerLeave(const PointerEvent& event) {
    if (event.hasPointerCapture(*this)) return false;
    const bool changed = m_chrome.pointerMove(-1);
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::onPointerMove(const PointerEvent& event) {
    if (event.source != PointerSource::Mouse && !event.hasPointerCapture(*this)) {
        return false;
    }
    const bool changed = m_chrome.pointerMove(hitTest(event));
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::onPointerDown(const PointerEvent& event) {
    if (event.button != 0) return false;
    const int controlIndex = hitTest(event);
    if (controlIndex >= 0) {
        if (!m_chrome.pointerDown(controlIndex)) return false;
        event.capturePointer(*this);
        scheduleChromePresentation();
        return true;
    }
    return m_actions.beginDrag ? m_actions.beginDrag(event.x, event.y) : false;
}

bool WindowChromeSurface::onPointerUp(const PointerEvent& event) {
    if (!event.hasPointerCapture(*this)) return false;
    const int controlIndex = hitTest(event);
    const int activated = m_chrome.pointerUp(
        controlIndex, event.source == PointerSource::Mouse && controlIndex >= 0);
    event.releasePointerCapture(*this);
    scheduleChromePresentation();
    return activated < 0 ? true : activate(static_cast<size_t>(activated));
}

bool WindowChromeSurface::onPointerCancel(const PointerEvent& event) {
    event.releasePointerCapture(*this);
    const bool changed = m_chrome.cancelPointer();
    if (changed) scheduleChromePresentation();
    return changed;
}

void WindowChromeSurface::scheduleChromePresentation() {
    invalidatePaint();
    if (!m_motionCoordinator || !m_chrome.hasActiveAnimations()) return;
    m_motionCoordinator->registerPresentation(*this, [this](float deltaSec) {
        if (m_chrome.tick(deltaSec)) invalidatePaint();
        if (!m_chrome.hasActiveAnimations() && m_motionCoordinator) {
            m_motionCoordinator->unregisterPresentation(getObjectId());
        }
    });
}

bool WindowChromeSurface::activate(size_t index) {
    switch (lcl::chrome::WindowChromeWidget::actionForControl(index)) {
        case lcl::chrome::WindowChromeAction::Close:
            return m_actions.close ? m_actions.close() : true;
        case lcl::chrome::WindowChromeAction::Minimize:
            return m_actions.minimize ? m_actions.minimize() : true;
        case lcl::chrome::WindowChromeAction::ToggleMaximize:
            return m_actions.toggleMaximize ? m_actions.toggleMaximize() : true;
    }
    return false;
}

WindowTitlebarLayout calculateWindowTitlebarLayout(
        float width, float titleHeight, float cornerRadius, float titleFontSize,
        const WindowChromeStyle& style) {
    return lcl::chrome::WindowChromeWidget({}, style).layout(
        width, titleHeight, cornerRadius, titleFontSize);
}

std::unique_ptr<WindowChromeSurface> buildWindowTitlebar(
        float width, float titleHeight, float cornerRadius,
        const std::string& title, float titleFontSize,
        const WindowChromeStyle& style, WindowChromeActions actions) {
    return std::make_unique<WindowChromeSurface>(
        width, titleHeight, cornerRadius, title, titleFontSize, style,
        std::move(actions));
}

} // namespace lcl::ui::chrome
