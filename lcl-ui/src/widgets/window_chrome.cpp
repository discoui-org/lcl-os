#include "lcl-ui/widgets/window_chrome.hpp"

#include <algorithm>
#include <utility>

namespace lcl::ui::chrome {

namespace {
graphics::Color toUiColor(const graphics::Color& color) {
    return color;
}

void applyColor(Container& control, AnimatableProperty first,
                const graphics::Color& color) {
    control.applyPresentationValue(first, color.r);
    control.applyPresentationValue(
        static_cast<AnimatableProperty>(static_cast<int>(first) + 1), color.g);
    control.applyPresentationValue(
        static_cast<AnimatableProperty>(static_cast<int>(first) + 2), color.b);
    control.applyPresentationValue(
        static_cast<AnimatableProperty>(static_cast<int>(first) + 3), color.a);
}
} // namespace

WindowControl::WindowControl(WindowChromeSurface& owner, size_t index,
                             const WindowChromeStyle& style)
    : m_owner(owner), m_index(index) {
    setBackgroundColor(toUiColor(style.buttonBackground));
    setBorderColor(toUiColor(style.buttonBorder));
    setBorderWidth(style.buttonBorderWidth);
    setBorderRadius(style.controlSize * 0.5f);
    setBorderRoundness(style.buttonRoundness);
}

bool WindowControl::onPointerEnter(const PointerEvent& event) {
    if (event.source != PointerSource::Mouse) return false;
    return m_owner.controlPointerMove(m_index, true);
}

bool WindowControl::onPointerLeave(const PointerEvent& event) {
    (void)event;
    return m_owner.controlPointerMove(m_index, false);
}

bool WindowControl::onPointerDown(const PointerEvent& event) {
    if (event.button != 0 || !m_owner.controlPointerDown(m_index)) return false;
    event.capturePointer(*this);
    return true;
}

bool WindowControl::onPointerMove(const PointerEvent& event) {
    if (!event.hasPointerCapture(*this)) return false;
    return m_owner.controlPointerMove(
        m_index, containsPresentationPoint(event.x, event.y));
}

bool WindowControl::onPointerUp(const PointerEvent& event) {
    const bool inside = containsPresentationPoint(event.x, event.y);
    const bool handled = m_owner.controlPointerUp(
        m_index, inside, event.source == PointerSource::Mouse);
    event.releasePointerCapture(*this);
    return handled;
}

bool WindowControl::onPointerCancel(const PointerEvent& event) {
    event.releasePointerCapture(*this);
    return m_owner.controlPointerCancel();
}

WindowChromeSurface::WindowChromeSurface(
        float width, float titleHeight, float cornerRadius, std::string title,
        float titleFontSize, WindowChromeStyle style,
        WindowChromeActions actions)
    : m_chrome(std::move(title), style), m_actions(std::move(actions)),
      m_layout(m_chrome.layout(width, titleHeight, cornerRadius,
                               titleFontSize)) {
    setBackgroundColor({0, 0, 0, 0});
    setBorderRadius(0.0f);
    getYogaNode().setPositionType(YGPositionTypeAbsolute);
    getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    getYogaNode().setPosition(YGEdgeTop, 0.0f);
    getYogaNode().setWidth(width);
    getYogaNode().setHeight(titleHeight);

    auto background = std::make_unique<Container>();
    background->setBackgroundColor(toUiColor(style.titleBarBackground));
    background->setBorderRadius(std::clamp(
        cornerRadius + style.titleBarCornerRadiusAdjust, 0.0f,
        std::max(0.0f, titleHeight)));
    background->setTopOnlyBorderRadius(true);
    background->setBorderRoundness(style.titleBarRoundness);
    background->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    background->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    background->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    background->getYogaNode().setWidth(width);
    background->getYogaNode().setHeight(titleHeight);
    addChild(std::move(background));

    for (size_t index = 0; index < m_controls.size(); ++index) {
        auto controlWidget = std::make_unique<WindowControl>(*this, index, style);
        m_controls[index] = controlWidget.get();
        controlWidget->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        controlWidget->getYogaNode().setPosition(
            YGEdgeLeft, m_layout.controlLeft + static_cast<float>(index) *
                (style.controlSize + style.controlGap));
        controlWidget->getYogaNode().setPosition(YGEdgeTop, m_layout.controlTop);
        controlWidget->getYogaNode().setWidth(style.controlSize);
        controlWidget->getYogaNode().setHeight(style.controlSize);
        addChild(std::move(controlWidget));
    }

    auto titleText = std::make_unique<Text>(truncateTitleToWidth(
        m_chrome.title(), m_layout.titleWidth, titleFontSize));
    titleText->setTextColor(toUiColor(style.titleColor));
    titleText->setFontSize(titleFontSize);
    titleText->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titleText->getYogaNode().setPosition(YGEdgeLeft, m_layout.titleLeft);
    titleText->getYogaNode().setPosition(YGEdgeTop, m_layout.titleTop);
    titleText->getYogaNode().setWidth(m_layout.titleWidth);
    addChild(std::move(titleText));

    syncControlPresentation();
}

bool WindowChromeSurface::controlPointerMove(size_t index, bool inside) {
    const bool changed = m_chrome.pointerMove(inside ? static_cast<int>(index) : -1);
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::controlPointerDown(size_t index) {
    const bool changed = m_chrome.pointerDown(static_cast<int>(index));
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::controlPointerUp(size_t index, bool inside,
                                           bool keepHovered) {
    const int activated = m_chrome.pointerUp(
        inside ? static_cast<int>(index) : -1, keepHovered && inside);
    scheduleChromePresentation();
    if (activated < 0) return true;
    return activate(static_cast<size_t>(activated));
}

bool WindowChromeSurface::controlPointerCancel() {
    const bool changed = m_chrome.cancelPointer();
    if (changed) scheduleChromePresentation();
    return changed;
}

bool WindowChromeSurface::onPointerDown(const PointerEvent& event) {
    if (event.button != 0 || !m_actions.beginDrag) return false;
    return m_actions.beginDrag(event.x, event.y);
}

void WindowChromeSurface::syncControlPresentation() {
    for (size_t index = 0; index < m_controls.size(); ++index) {
        auto* controlWidget = m_controls[index];
        const auto visual = m_chrome.visual(index);
        controlWidget->applyPresentationValue(AnimatableProperty::ScaleX,
                                              visual.scale);
        controlWidget->applyPresentationValue(AnimatableProperty::ScaleY,
                                              visual.scale);
        applyColor(*controlWidget, AnimatableProperty::BackgroundRed,
                   visual.background);
        applyColor(*controlWidget, AnimatableProperty::BorderRed,
                   visual.border);
    }
}

void WindowChromeSurface::scheduleChromePresentation() {
    syncControlPresentation();
    if (!m_motionCoordinator || !m_chrome.hasActiveAnimations()) return;
    m_motionCoordinator->registerPresentation(*this, [this](float deltaSec) {
        if (m_chrome.tick(deltaSec)) syncControlPresentation();
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

std::string truncateTitleToWidth(const std::string& title, float widthPx,
                                 float fontSizePx) {
    std::string out = title;
    if (widthPx <= 0.0f) return {};
    const float approxCharWidth = std::max(1.0f, fontSizePx * 0.6f);
    const int maxCharacters = static_cast<int>(widthPx / approxCharWidth);
    if (maxCharacters <= 0) return {};
    if (static_cast<int>(out.size()) > maxCharacters) {
        if (maxCharacters <= 3) {
            out.resize(static_cast<size_t>(maxCharacters));
        } else {
            out.resize(static_cast<size_t>(maxCharacters - 3));
            out += "...";
        }
    }
    return out;
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
