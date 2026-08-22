#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "lcl-window-chrome/window_chrome.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"

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

/** lcl-ui host for one control owned by the shared WindowChromeWidget. */
class WindowControl final : public Container {
public:
    WindowControl(WindowChromeSurface& owner, size_t index,
                  const WindowChromeStyle& style);

    size_t controlIndex() const noexcept { return m_index; }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;

private:
    WindowChromeSurface& m_owner;
    size_t m_index{0};
};

/** CSD host for the same chrome state used by the compositor's SSD path. */
class WindowChromeSurface final : public Container {
public:
    WindowChromeSurface(float width, float titleHeight, float cornerRadius,
                        std::string title, float titleFontSize,
                        WindowChromeStyle style = {},
                        WindowChromeActions actions = {});

    lcl::chrome::WindowChromeWidget& sharedChrome() noexcept { return m_chrome; }
    const lcl::chrome::WindowChromeWidget& sharedChrome() const noexcept {
        return m_chrome;
    }
    WindowControl* control(size_t index) const { return m_controls.at(index); }
    const WindowTitlebarLayout& chromeLayout() const noexcept { return m_layout; }

    bool controlPointerMove(size_t index, bool inside);
    bool controlPointerDown(size_t index);
    bool controlPointerUp(size_t index, bool inside, bool keepHovered);
    bool controlPointerCancel();
    bool onPointerDown(const PointerEvent& event) override;

private:
    void syncControlPresentation();
    void scheduleChromePresentation();
    bool activate(size_t index);

    lcl::chrome::WindowChromeWidget m_chrome;
    WindowChromeActions m_actions;
    WindowTitlebarLayout m_layout{};
    std::array<WindowControl*, 3> m_controls{};
};

WindowTitlebarLayout calculateWindowTitlebarLayout(
    float width, float titleHeight, float cornerRadius, float titleFontSize,
    const WindowChromeStyle& style = {});

std::string truncateTitleToWidth(const std::string& title, float widthPx,
                                 float fontSizePx);

std::unique_ptr<WindowChromeSurface> buildWindowTitlebar(
    float width, float titleHeight, float cornerRadius, const std::string& title,
    float titleFontSize, const WindowChromeStyle& style = {},
    WindowChromeActions actions = {});

} // namespace lcl::ui::chrome
