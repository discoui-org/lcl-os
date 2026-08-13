#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "lcl-motion/motion.hpp"

namespace lcl::render {

struct WindowChromeColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};
};

struct WindowChromeStyle {
    float controlSize{16.0f};
    float controlGap{6.0f};
    float minInset{8.0f};
    float cornerInsetOffset{8.0f};
    float titleGapAfterControls{12.0f};
    float titleMinLeft{14.0f};
    float titleRightPadding{10.0f};
    float borderWidth{1.0f};
    float roundness{2.0f};
    WindowChromeColor normalBackground{235, 241, 248, 56};
    WindowChromeColor normalBorder{230, 238, 248, 120};
    WindowChromeColor hoverBackground{245, 249, 255, 84};
    WindowChromeColor hoverBorder{242, 248, 255, 168};
    WindowChromeColor pressedBackground{218, 226, 238, 112};
    WindowChromeColor pressedBorder{250, 252, 255, 208};
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

/**
 * Compositor-owned titlebar widget shared by every decorated window.
 *
 * A Window is the parent group; this widget and the client surface are its two
 * children. Client processes never draw or hit-test built-in window controls.
 */
class WindowChromeWidget {
public:
    explicit WindowChromeWidget(std::string title = {});

    void setTitle(std::string title) { m_title = std::move(title); }
    const std::string& title() const noexcept { return m_title; }
    const WindowChromeStyle& style() const noexcept { return m_style; }

    WindowChromeLayout layout(float width, float titleHeight, float cornerRadius,
                              float fontSize, float displayScale) const;
    int hitTest(float localX, float localY, float width, float titleHeight,
                float cornerRadius, float displayScale) const;

    bool pointerMove(int controlIndex);
    bool pointerDown(int controlIndex);
    /** Returns the activated control, or -1 when the press was cancelled. */
    int pointerUp(int controlIndex);
    bool cancelPointer();
    bool tick(float dtSec);

    int hoveredControl() const noexcept { return m_hoveredControl; }
    int pressedControl() const noexcept { return m_pressedControl; }
    const WindowControlPresentation& control(size_t index) const { return m_controls.at(index); }

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

} // namespace lcl::render
