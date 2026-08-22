#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "lcl-motion/motion.hpp"

namespace lcl::chrome {

struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};
};

struct WindowChromeStyle {
    float controlSize{16.0f};
    float controlGap{6.0f};
    float controlLeftRadiusOffset{8.0f};
    float minControlLeft{8.0f};
    float minControlTop{4.0f};
    float titleGapAfterControls{12.0f};
    float titleMinLeft{14.0f};
    float titleRightPadding{10.0f};
    float titleBarCornerRadiusAdjust{0.0f};
    float titleBarRoundness{2.0f};

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

struct WindowChromeLayout {
    float controlLeft{0.0f};
    float controlTop{0.0f};
    float titleLeft{0.0f};
    float titleTop{0.0f};
    float titleWidth{0.0f};
};

struct WindowControlPresentation {
    float scale{1.0f};
    float emphasis{0.0f}; // 0 normal, 1 hover, 2 pressed
};

struct WindowControlVisual {
    float scale{1.0f};
    Color background{};
    Color border{};
};

enum class WindowChromeAction : uint8_t {
    Close,
    Minimize,
    ToggleMaximize,
};

/**
 * Renderer-independent window chrome state shared by CSD clients and SSD.
 * Hosts provide input coordinates, animation ticks, and their own painter.
 */
class WindowChromeWidget {
public:
    explicit WindowChromeWidget(std::string title = {},
                                WindowChromeStyle style = {});

    void setTitle(std::string title) { m_title = std::move(title); }
    const std::string& title() const noexcept { return m_title; }
    const WindowChromeStyle& style() const noexcept { return m_style; }

    WindowChromeLayout layout(float width, float titleHeight, float cornerRadius,
                              float fontSize, float displayScale = 1.0f) const;
    int hitTest(float localX, float localY, float width, float titleHeight,
                float cornerRadius, float displayScale = 1.0f) const;

    bool pointerMove(int controlIndex);
    bool pointerDown(int controlIndex);
    /** Returns the activated control, or -1 when the press was cancelled. */
    int pointerUp(int controlIndex, bool keepHovered = true);
    bool cancelPointer();
    bool tick(float dtSec);
    bool hasActiveAnimations() const noexcept;

    int hoveredControl() const noexcept { return m_hoveredControl; }
    int pressedControl() const noexcept { return m_pressedControl; }
    const WindowControlPresentation& control(size_t index) const {
        return m_controls.at(index);
    }
    WindowControlVisual visual(size_t index) const;

    static WindowChromeAction actionForControl(size_t index);

private:
    void animateControl(size_t index, float targetScale, float targetEmphasis,
                        const lcl::motion::Motion& motion);

    std::string m_title;
    WindowChromeStyle m_style{};
    int m_hoveredControl{-1};
    int m_pressedControl{-1};
    std::array<WindowControlPresentation, 3> m_controls{};
    lcl::motion::AnimationEngine m_motion;
};

} // namespace lcl::chrome
