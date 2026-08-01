#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"

namespace lcl::ui::chrome {

struct HeaderControlsStyle {
    float controlSize{16.0f};
    float controlGap{6.0f};
    float controlLeftRadiusOffset{8.0f};
    float minControlLeft{8.0f};
    float minControlTop{4.0f};
    float titleGapAfterControls{12.0f};
    float titleMinLeft{14.0f};
    float titleRightPadding{10.0f};

    Color buttonBackground{235, 241, 248, 40};
    Color buttonBorder{230, 238, 248, 92};
    Color buttonGlyph{232, 240, 248, 210};
    Color titleColor{230, 245, 255, 220};

    float buttonBorderWidth{1.0f};
    float buttonRoundness{2.0f};
    float glyphFontSize{11.0f};
    float glyphLeftFactor{0.32f};
    float glyphTopFactor{0.16f};
};

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

inline std::unique_ptr<Container> buildLibadwaitaTitleBar(float width,
                                                           float titleHeight,
                                                           float cornerRadius,
                                                           const std::string& title,
                                                           float titleFontSize,
                                                           const HeaderControlsStyle& style = HeaderControlsStyle{}) {
    auto titleBar = std::make_unique<Container>();
    titleBar->setBackgroundColor(Color{255, 255, 255, 0});
    titleBar->setBorderRadius(0.0f);
    titleBar->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titleBar->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    titleBar->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    titleBar->getYogaNode().setWidth(width);
    titleBar->getYogaNode().setHeight(titleHeight);

    const float ctrlLeft = std::max(style.minControlLeft, cornerRadius - style.controlLeftRadiusOffset);
    const float ctrlTop = std::max(style.minControlTop, (titleHeight - style.controlSize) * 0.5f);
    const float titleStartX = std::max(
        style.titleMinLeft,
        ctrlLeft + (style.controlSize * 3.0f) + (style.controlGap * 2.0f) + style.titleGapAfterControls);
    const float titleAvailW = std::max(0.0f, width - titleStartX - style.titleRightPadding);

    auto mkHeaderControl = [&](float left, const char* glyph) {
        auto button = std::make_unique<Container>();
        button->setBackgroundColor(style.buttonBackground);
        button->setBorderColor(style.buttonBorder);
        button->setBorderWidth(style.buttonBorderWidth);
        button->setBorderRadius(style.controlSize * 0.5f);
        button->setBorderRoundness(style.buttonRoundness);
        button->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        button->getYogaNode().setPosition(YGEdgeLeft, left);
        button->getYogaNode().setPosition(YGEdgeTop, ctrlTop);
        button->getYogaNode().setWidth(style.controlSize);
        button->getYogaNode().setHeight(style.controlSize);

        auto icon = std::make_unique<Text>(glyph);
        icon->setTextColor(style.buttonGlyph);
        icon->setFontSize(style.glyphFontSize);
        icon->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        icon->getYogaNode().setPosition(YGEdgeLeft, style.controlSize * style.glyphLeftFactor);
        icon->getYogaNode().setPosition(YGEdgeTop, style.controlSize * style.glyphTopFactor);
        button->addChild(std::move(icon));

        return button;
    };

    titleBar->addChild(mkHeaderControl(ctrlLeft, "x"));
    titleBar->addChild(mkHeaderControl(ctrlLeft + style.controlSize + style.controlGap, "-"));
    titleBar->addChild(mkHeaderControl(ctrlLeft + (style.controlSize + style.controlGap) * 2.0f, "+"));

    std::string titleLabel = truncateTitleToWidth(title, titleAvailW, titleFontSize);
    auto titleText = std::make_unique<Text>(titleLabel);
    titleText->setTextColor(style.titleColor);
    titleText->setFontSize(titleFontSize);
    titleText->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titleText->getYogaNode().setPosition(YGEdgeLeft, titleStartX);
    titleText->getYogaNode().setPosition(YGEdgeTop, std::max(0.0f, (titleHeight - titleFontSize) * 0.45f));
    titleText->getYogaNode().setWidth(titleAvailW);
    titleBar->addChild(std::move(titleText));

    return titleBar;
}

} // namespace lcl::ui::chrome
