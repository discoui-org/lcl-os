#pragma once

#include <functional>
#include <memory>
#include <string>

#include "lcl-window-chrome/window_chrome.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui::chrome {

using WindowChromeStyle = lcl::chrome::WindowChromeStyle;
using WindowTitlebarLayout = lcl::chrome::WindowChromeLayout;

struct WindowChromeActions {
    std::function<bool()> close;
    std::function<bool()> minimize;
    std::function<bool()> toggleMaximize;
    std::function<bool(float, float)> beginDrag;
};

class WindowChromeSurface;

/** CSD host that paints and hit-tests through the shared SSD chrome core. */
class WindowChromeSurface final : public Widget {
public:
    WindowChromeSurface(float width, float titleHeight, float cornerRadius,
                        std::string title, float titleFontSize,
                        WindowChromeStyle style = {},
                        WindowChromeActions actions = {});

    lcl::chrome::WindowChromeWidget& sharedChrome() noexcept { return m_chrome; }
    const lcl::chrome::WindowChromeWidget& sharedChrome() const noexcept {
        return m_chrome;
    }
    const WindowTitlebarLayout& chromeLayout() const noexcept { return m_layout; }
    /** Resize the mounted chrome without replacing its widget/event tree. */
    void setFrameSize(float width, float height);
    graphics::DisplayList buildChromeDisplayList(
        const graphics::RectF& bounds) const;

    void syncLayout(float parentAbsX = 0.0f,
                    float parentAbsY = 0.0f) override;
    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;
    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;

private:
    int hitTest(const PointerEvent& event) const;
    void scheduleChromePresentation();
    bool activate(size_t index);

    lcl::chrome::WindowChromeWidget m_chrome;
    WindowChromeActions m_actions;
    WindowTitlebarLayout m_layout{};
    float m_titleHeight{0.0f};
    float m_cornerRadius{0.0f};
    float m_titleFontSize{0.0f};
};

WindowTitlebarLayout calculateWindowTitlebarLayout(
    float width, float titleHeight, float cornerRadius, float titleFontSize,
    const WindowChromeStyle& style = {});

std::unique_ptr<WindowChromeSurface> buildWindowTitlebar(
    float width, float titleHeight, float cornerRadius, const std::string& title,
    float titleFontSize, const WindowChromeStyle& style = {},
    WindowChromeActions actions = {});

} // namespace lcl::ui::chrome
