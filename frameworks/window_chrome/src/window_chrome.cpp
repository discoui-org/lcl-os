#include "lcl-window-chrome/window_chrome.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace lcl::chrome {

namespace {
constexpr uint32_t kScalePropertyBase = 100;
constexpr uint32_t kEmphasisPropertyBase = 110;

uint8_t mixedByte(uint8_t normal, uint8_t hover, uint8_t pressed,
                  float emphasis) {
    const float hoverMix = std::min(1.0f, std::clamp(emphasis, 0.0f, 2.0f));
    const float pressedMix = std::max(0.0f, std::clamp(emphasis, 0.0f, 2.0f) - 1.0f);
    const float hoverValue = static_cast<float>(normal) +
        (static_cast<float>(hover) - static_cast<float>(normal)) * hoverMix;
    return static_cast<uint8_t>(std::clamp(std::lround(
        hoverValue + (static_cast<float>(pressed) - hoverValue) * pressedMix),
        0l, 255l));
}

lcl::graphics::Color mixedColor(const lcl::graphics::Color& normal,
                                const lcl::graphics::Color& hover,
                                const lcl::graphics::Color& pressed,
                                float emphasis) {
    return {
        mixedByte(normal.r, hover.r, pressed.r, emphasis),
        mixedByte(normal.g, hover.g, pressed.g, emphasis),
        mixedByte(normal.b, hover.b, pressed.b, emphasis),
        mixedByte(normal.a, hover.a, pressed.a, emphasis),
    };
}
} // namespace

WindowChromeWidget::WindowChromeWidget(std::string title,
                                       WindowChromeStyle style)
    : m_title(std::move(title)), m_style(std::move(style)) {}

WindowChromeLayout WindowChromeWidget::layout(float width, float titleHeight,
                                               float cornerRadius,
                                               float fontSize) const {
    const float controlSize = m_style.controlSize;
    const float controlGap = m_style.controlGap;
    const float inset = std::max({m_style.minControlLeft,
        cornerRadius - m_style.controlLeftRadiusOffset,
        m_style.minControlTop});
    const float titleLeft = std::max(m_style.titleMinLeft,
        inset + controlSize * 3.0f + controlGap * 2.0f +
            m_style.titleGapAfterControls);
    return {
        inset,
        inset,
        titleLeft,
        std::clamp(inset + (controlSize - fontSize) * 0.5f, 0.0f,
                   std::max(0.0f, titleHeight - fontSize)),
        std::max(0.0f, width - titleLeft -
            m_style.titleRightPadding),
    };
}

int WindowChromeWidget::hitTest(float localX, float localY, float width,
                                float titleHeight, float cornerRadius) const {
    const auto chromeLayout = layout(width, titleHeight, cornerRadius,
                                     15.0f);
    const float controlSize = m_style.controlSize;
    const float controlGap = m_style.controlGap;
    if (localY < chromeLayout.controlTop ||
        localY > chromeLayout.controlTop + controlSize) {
        return -1;
    }
    for (int index = 0; index < 3; ++index) {
        const float left = chromeLayout.controlLeft +
            static_cast<float>(index) * (controlSize + controlGap);
        if (localX >= left && localX <= left + controlSize) return index;
    }
    return -1;
}

lcl::graphics::DisplayList WindowChromeWidget::buildDisplayList(
        const WindowChromePaintOptions& options) const {
    lcl::graphics::DisplayListBuilder builder;
    const auto chromeLayout = layout(options.bounds.width, options.titleHeight,
                                     options.cornerRadius, options.fontSize);
    auto withOpacity = [&](lcl::graphics::Color color) {
        color.a = static_cast<uint8_t>(std::clamp(std::lround(
            static_cast<float>(color.a) * std::clamp(options.opacity, 0.0f, 1.0f)),
            0l, 255l));
        return color;
    };

    if (options.drawTitlebar && options.titlebarColor.a > 0) {
        lcl::graphics::Path titlebar;
        const float r = std::clamp(
            options.cornerRadius + m_style.titleBarCornerRadiusAdjust,
            0.0f, options.titleHeight);
        titlebar.addTopRRect({options.bounds.x, options.bounds.y,
                              options.bounds.width, options.titleHeight},
                             r, m_style.titleBarRoundness);
        builder.drawPath(titlebar, {{withOpacity(options.titlebarColor)},
                                    lcl::graphics::PaintStyle::Fill});
    }

    for (size_t index = 0; index < m_controls.size(); ++index) {
        const auto visualState = visual(index);
        const float cx = options.bounds.x + chromeLayout.controlLeft +
            static_cast<float>(index) * (m_style.controlSize + m_style.controlGap) +
            m_style.controlSize * 0.5f;
        const float cy = options.bounds.y + chromeLayout.controlTop +
            m_style.controlSize * 0.5f;
        builder.save();
        builder.concat(lcl::graphics::Matrix3::translation(cx, cy));
        builder.concat(lcl::graphics::Matrix3::scale(visualState.scale,
                                                      visualState.scale));
        builder.concat(lcl::graphics::Matrix3::translation(-cx, -cy));
        lcl::graphics::Path controlPath;
        controlPath.addEllipse({cx - m_style.controlSize * 0.5f,
                                cy - m_style.controlSize * 0.5f,
                                m_style.controlSize, m_style.controlSize});
        builder.drawPath(controlPath, {{withOpacity(visualState.background)},
                                       lcl::graphics::PaintStyle::Fill});
        lcl::graphics::Paint stroke{};
        stroke.color = withOpacity(visualState.border);
        stroke.style = lcl::graphics::PaintStyle::Stroke;
        stroke.stroke.width = m_style.buttonBorderWidth;
        stroke.stroke.join = lcl::graphics::StrokeJoin::Round;
        builder.drawPath(controlPath, stroke);
        builder.restore();
    }

    if (options.drawTitlebar) {
        const int maxCharacters = static_cast<int>(
            chromeLayout.titleWidth / std::max(1.0f, options.fontSize * 0.6f));
        std::string visibleTitle = m_title;
        if (maxCharacters <= 0) {
            visibleTitle.clear();
        } else if (static_cast<int>(visibleTitle.size()) > maxCharacters) {
            visibleTitle = maxCharacters <= 3
                ? visibleTitle.substr(0, static_cast<size_t>(maxCharacters))
                : visibleTitle.substr(0, static_cast<size_t>(maxCharacters - 3)) + "...";
        }
        builder.drawText(
            {options.bounds.x + chromeLayout.titleLeft,
             options.bounds.y + chromeLayout.titleTop},
            std::move(visibleTitle), withOpacity(m_style.titleColor), options.fontSize);
    }
    return builder.build();
}

void WindowChromeWidget::animateControl(size_t index, float targetScale,
                                        float targetEmphasis,
                                        const lcl::motion::Motion& motion) {
    const auto scaleChannel = m_motion.ensureChannel(
        {1u, kScalePropertyBase + static_cast<uint32_t>(index)},
        m_controls[index].scale);
    const auto emphasisChannel = m_motion.ensureChannel(
        {1u, kEmphasisPropertyBase + static_cast<uint32_t>(index)},
        m_controls[index].emphasis);
    m_motion.animateTo(scaleChannel, targetScale, motion);
    m_motion.animateTo(emphasisChannel, targetEmphasis, motion);
}

bool WindowChromeWidget::pointerMove(int controlIndex) {
    controlIndex = std::clamp(controlIndex, -1, 2);
    if (m_hoveredControl == controlIndex) return false;
    const int oldHover = m_hoveredControl;
    m_hoveredControl = controlIndex;
    if (oldHover >= 0 && oldHover != m_pressedControl) {
        animateControl(static_cast<size_t>(oldHover), 1.0f, 0.0f,
                       lcl::motion::tokens::release());
    }
    if (controlIndex >= 0 && controlIndex != m_pressedControl) {
        animateControl(static_cast<size_t>(controlIndex), 1.015f, 1.0f,
                       lcl::motion::tokens::hover());
    }
    return true;
}

bool WindowChromeWidget::pointerDown(int controlIndex) {
    controlIndex = std::clamp(controlIndex, -1, 2);
    if (controlIndex < 0 || m_pressedControl == controlIndex) return false;
    if (m_pressedControl >= 0) {
        animateControl(static_cast<size_t>(m_pressedControl), 1.0f, 0.0f,
                       lcl::motion::tokens::release());
    }
    m_pressedControl = controlIndex;
    m_hoveredControl = controlIndex;
    animateControl(static_cast<size_t>(controlIndex), 0.965f, 2.0f,
                   lcl::motion::tokens::pressed());
    return true;
}

int WindowChromeWidget::pointerUp(int controlIndex, bool keepHovered) {
    controlIndex = std::clamp(controlIndex, -1, 2);
    const int pressed = m_pressedControl;
    if (pressed < 0) return -1;
    m_pressedControl = -1;
    m_hoveredControl = keepHovered ? controlIndex : -1;
    const bool activated = pressed == controlIndex;
    const bool remainsHovered = activated && keepHovered;
    animateControl(static_cast<size_t>(pressed), remainsHovered ? 1.015f : 1.0f,
                   remainsHovered ? 1.0f : 0.0f,
                   lcl::motion::tokens::release());
    return activated ? pressed : -1;
}

bool WindowChromeWidget::cancelPointer() {
    if (m_hoveredControl < 0 && m_pressedControl < 0) return false;
    const int oldHover = m_hoveredControl;
    const int oldPress = m_pressedControl;
    m_hoveredControl = -1;
    m_pressedControl = -1;
    for (int index = 0; index < 3; ++index) {
        if (index == oldHover || index == oldPress) {
            animateControl(static_cast<size_t>(index), 1.0f, 0.0f,
                           lcl::motion::tokens::release());
        }
    }
    return true;
}

bool WindowChromeWidget::tick(float dtSec) {
    const auto changed = m_motion.tick(dtSec);
    bool presentationChanged = false;
    for (size_t index = 0; index < m_controls.size(); ++index) {
        const auto scaleChannel = m_motion.findChannel(
            {1u, kScalePropertyBase + static_cast<uint32_t>(index)});
        const auto emphasisChannel = m_motion.findChannel(
            {1u, kEmphasisPropertyBase + static_cast<uint32_t>(index)});
        if (!scaleChannel || !emphasisChannel) continue;
        const float scale = m_motion.sample(*scaleChannel).value;
        const float emphasis = m_motion.sample(*emphasisChannel).value;
        if (std::fabs(scale - m_controls[index].scale) > 0.0001f ||
            std::fabs(emphasis - m_controls[index].emphasis) > 0.0001f) {
            m_controls[index] = {scale, emphasis};
            presentationChanged = true;
        }
    }
    return presentationChanged || !changed.empty();
}

bool WindowChromeWidget::hasActiveAnimations() const noexcept {
    return m_motion.hasActiveAnimations();
}

WindowControlVisual WindowChromeWidget::visual(size_t index) const {
    const auto& presentation = m_controls.at(index);
    return {
        presentation.scale,
        mixedColor(m_style.buttonBackground, m_style.buttonHoverBackground,
                   m_style.buttonPressedBackground, presentation.emphasis),
        mixedColor(m_style.buttonBorder, m_style.buttonHoverBorder,
                   m_style.buttonPressedBorder, presentation.emphasis),
    };
}

WindowChromeAction WindowChromeWidget::actionForControl(size_t index) {
    switch (index) {
        case 0: return WindowChromeAction::Close;
        case 1: return WindowChromeAction::Minimize;
        case 2: return WindowChromeAction::ToggleMaximize;
        default: throw std::out_of_range("window chrome control index");
    }
}

} // namespace lcl::chrome
