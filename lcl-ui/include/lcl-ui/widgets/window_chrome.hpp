#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"

namespace lcl::ui::chrome {

struct WindowChromeStyle {
    float controlSize{16.0f};
    float controlGap{6.0f};
    float controlLeftRadiusOffset{8.0f};
    float minControlLeft{8.0f};
    float minControlTop{4.0f};
    float titleGapAfterControls{12.0f};
    float titleMinLeft{14.0f};
    float titleRightPadding{10.0f};
    // Must match the outer window mask exactly; a smaller top radius leaves
    // titlebar pixels visible outside the window silhouette at HiDPI scales.
    float titleBarCornerRadiusAdjust{0.0f};
    float titleBarRoundness{2.0f};

    // Controls are the same widgets in CSD and SSD. The renderer selects the
    // antialiasing backend; style values stay independent of that backend.
    Color buttonBackground{235, 241, 248, 56};
    Color buttonBorder{230, 238, 248, 120};
    Color buttonHoverBackground{245, 249, 255, 84};
    Color buttonHoverBorder{242, 248, 255, 168};
    Color buttonPressedBackground{218, 226, 238, 112};
    Color buttonPressedBorder{250, 252, 255, 208};
    Color titleColor{240, 248, 255, 245};
    Color titleBarBackground{0, 0, 0, 0};

    float buttonBorderWidth{1.0f};
    float buttonRoundness{2.0f};
};

/** Glyph-free window control with the standard hover/press motion tokens. */
class WindowControl final : public Container {
public:
    explicit WindowControl(WindowChromeStyle style) : m_style(std::move(style)) {
        setBackgroundColor(m_style.buttonBackground);
        setBorderColor(m_style.buttonBorder);
        setBorderWidth(m_style.buttonBorderWidth);
        setBorderRadius(m_style.controlSize * 0.5f);
        setBorderRoundness(m_style.buttonRoundness);
    }

    bool onPointerEnter(const PointerEvent&) override {
        m_hovered = true;
        if (!m_pressed) applyVisual(m_style.buttonHoverBackground, m_style.buttonHoverBorder,
                                    interactionMotionTheme().hoverScale,
                                    interactionMotionTheme().hover);
        return true;
    }

    bool onPointerLeave(const PointerEvent&) override {
        m_hovered = false;
        m_pressed = false;
        applyVisual(m_style.buttonBackground, m_style.buttonBorder, 1.0f,
                    interactionMotionTheme().release);
        return true;
    }

    bool onPointerDown(const PointerEvent&) override {
        m_pressed = true;
        applyVisual(m_style.buttonPressedBackground, m_style.buttonPressedBorder,
                    interactionMotionTheme().pressedScale,
                    interactionMotionTheme().pressed);
        return true;
    }

    bool onPointerUp(const PointerEvent& event) override {
        m_pressed = false;
        m_hovered = (event.source == PointerSource::Mouse);
        applyVisual(m_hovered ? m_style.buttonHoverBackground : m_style.buttonBackground,
                    m_hovered ? m_style.buttonHoverBorder : m_style.buttonBorder,
                    m_hovered ? interactionMotionTheme().hoverScale : 1.0f,
                    interactionMotionTheme().release);
        return true;
    }

    bool onPointerCancel(const PointerEvent&) override {
        m_pressed = false;
        m_hovered = false;
        applyVisual(m_style.buttonBackground, m_style.buttonBorder, 1.0f,
                    interactionMotionTheme().release);
        return true;
    }

private:
    void applyVisual(Color background, Color border, float scale,
                     const lcl::motion::Motion& scaleMotion) {
        const auto& theme = interactionMotionTheme();
        if (!theme.enabled || !m_motionCoordinator) {
            setBackgroundColor(background);
            setBorderColor(border);
            setScale(scale);
            return;
        }
        animateBackgroundColor(background, theme.focusTransition);
        animateBorderColor(border, theme.focusTransition);
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX,
            m_presentation.scaleX, scale, scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
        m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY,
            m_presentation.scaleY, scale, scaleMotion,
            [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
    }

    WindowChromeStyle m_style;
    bool m_hovered{false};
    bool m_pressed{false};
};

struct WindowTitlebarLayout {
    float controlLeft{0.0f};
    float controlTop{0.0f};
    float titleLeft{0.0f};
    float titleTop{0.0f};
    float titleWidth{0.0f};
};

inline WindowTitlebarLayout calculateWindowTitlebarLayout(float width,
                                                           float titleHeight,
                                                           float cornerRadius,
                                                           float titleFontSize,
                                                           const WindowChromeStyle& style = WindowChromeStyle{}) {
    const float radiusCenterInset = std::max(
        style.minControlLeft,
        cornerRadius - style.controlLeftRadiusOffset);
    const float controlInset = std::max(radiusCenterInset, style.minControlTop);
    const float titleLeft = std::max(
        style.titleMinLeft,
        controlInset + (style.controlSize * 3.0f) +
            (style.controlGap * 2.0f) + style.titleGapAfterControls);

    return {
        controlInset,
        controlInset,
        titleLeft,
        std::clamp(
            controlInset + (style.controlSize - titleFontSize) * 0.5f,
            0.0f,
            std::max(0.0f, titleHeight - titleFontSize)),
        std::max(0.0f, width - titleLeft - style.titleRightPadding),
    };
}

inline std::string truncateTitleToWidth(const std::string& title, float widthPx, float fontSizePx) {
    std::string out = title;
    if (widthPx <= 0.0f) {
        out.clear();
        return out;
    }

    const float approxCharW = std::max(1.0f, fontSizePx * 0.6f);
    const int maxChars = static_cast<int>(widthPx / approxCharW);
    if (maxChars <= 0) {
        out.clear();
        return out;
    }

    if (static_cast<int>(out.size()) > maxChars) {
        if (maxChars <= 3) {
            out = out.substr(0, static_cast<size_t>(maxChars));
        } else {
            out = out.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
        }
    }
    return out;
}

inline std::unique_ptr<Container> buildWindowTitlebar(float width,
                                                       float titleHeight,
                                                       float cornerRadius,
                                                       const std::string& title,
                                                       float titleFontSize,
                                                       const WindowChromeStyle& style = WindowChromeStyle{}) {
    auto titleBar = std::make_unique<Container>();
    titleBar->setBackgroundColor(Color{0, 0, 0, 0});
    titleBar->setBorderRadius(0.0f);
    titleBar->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titleBar->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    titleBar->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    titleBar->getYogaNode().setWidth(width);
    titleBar->getYogaNode().setHeight(titleHeight);

    // A title bar has only the window's upper arcs; its lower edge joins the
    // client area without rounding. Do not emulate this with a rectangular
    // strip: that spills outside the outer window curve when height < 2r.
    const float titleBarRadius = std::clamp(
        cornerRadius + style.titleBarCornerRadiusAdjust,
        0.0f,
        std::max(0.0f, titleHeight));

    auto bgRoundedTop = std::make_unique<Container>();
    bgRoundedTop->setBackgroundColor(style.titleBarBackground);
    bgRoundedTop->setBorderRadius(titleBarRadius);
    bgRoundedTop->setTopOnlyBorderRadius(true);
    bgRoundedTop->setBorderRoundness(style.titleBarRoundness);
    bgRoundedTop->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    bgRoundedTop->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    bgRoundedTop->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    bgRoundedTop->getYogaNode().setWidth(width);
    bgRoundedTop->getYogaNode().setHeight(titleHeight);

    titleBar->addChild(std::move(bgRoundedTop));

    const WindowTitlebarLayout layout = calculateWindowTitlebarLayout(
        width, titleHeight, cornerRadius, titleFontSize, style);

    auto mkHeaderControl = [&](float left) {
        auto button = std::make_unique<WindowControl>(style);
        button->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        button->getYogaNode().setPosition(YGEdgeLeft, left);
        button->getYogaNode().setPosition(YGEdgeTop, layout.controlTop);
        button->getYogaNode().setWidth(style.controlSize);
        button->getYogaNode().setHeight(style.controlSize);
        return button;
    };

    titleBar->addChild(mkHeaderControl(layout.controlLeft));
    titleBar->addChild(mkHeaderControl(layout.controlLeft + style.controlSize + style.controlGap));
    titleBar->addChild(mkHeaderControl(layout.controlLeft + (style.controlSize + style.controlGap) * 2.0f));

    std::string titleLabel = truncateTitleToWidth(title, layout.titleWidth, titleFontSize);

    auto titleText = std::make_unique<Text>(titleLabel);
    titleText->setTextColor(style.titleColor);
    titleText->setFontSize(titleFontSize);
    titleText->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titleText->getYogaNode().setPosition(YGEdgeLeft, layout.titleLeft);
    titleText->getYogaNode().setPosition(YGEdgeTop, layout.titleTop);
    titleText->getYogaNode().setWidth(layout.titleWidth);
    titleBar->addChild(std::move(titleText));

    return titleBar;
}

} // namespace lcl::ui::chrome
