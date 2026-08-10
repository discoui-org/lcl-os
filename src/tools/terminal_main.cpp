#include <algorithm>
#include <iostream>
#include <memory>

#include <linux/input-event-codes.h>

#include "apps/terminal/terminal_app.hpp"
#include "apps/terminal/terminal_view.hpp"
#include "core/input/key_mapper.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/window_chrome.hpp"

namespace {

constexpr uint32_t kSurfaceWidth = 540;
constexpr uint32_t kSurfaceHeight = 360;
constexpr float kTitlebarHeight = 34.0f;
constexpr float kCornerRadius = 20.0f;
constexpr float kTitleFontSize = 14.0f;

bool isTerminalControlKey(int keyCode, uint8_t modifiers) {
    if ((modifiers & lcl::core::LCL_MOD_CTRL) != 0) {
        return true;
    }

    switch (keyCode) {
        case KEY_ENTER:
        case KEY_KPENTER:
        case KEY_BACKSPACE:
        case KEY_TAB:
        case KEY_ESC:
        case KEY_UP:
        case KEY_DOWN:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_HOME:
        case KEY_END:
        case KEY_PAGEUP:
        case KEY_PAGEDOWN:
        case KEY_INSERT:
        case KEY_DELETE:
            return true;
        default:
            return false;
    }
}

} // namespace

int main() {
    std::cout << "====================================================\n"
              << "  lcl-terminal v0.1.0 - WindowApp Client          \n"
              << "====================================================\n";

    lcl::apps::TerminalApp terminal;
    if (!terminal.initialize(1)) {
        std::cerr << "[LCL Terminal ERROR] Terminal PTY initialization failed.\n";
        return 1;
    }

    lcl::ui::WindowApp window(kSurfaceWidth, kSurfaceHeight, "LCL Terminal");
    window.setInitialBounds(80, 60, kSurfaceWidth, kSurfaceHeight);
    window.setDecorationMode(lcl::protocol::LCLDecorationMode::CSD);
    window.setWindowCornerRadius(kCornerRadius);
    window.setCsdTitlebarEnabled(true);

    // CSD uses the same chrome geometry as compositor-owned titlebars.  The
    // WindowApp close hit target is derived from that shared first control.
    const lcl::ui::chrome::WindowChromeStyle chromeStyle;
    const auto titlebarLayout = lcl::ui::chrome::calculateWindowTitlebarLayout(
        static_cast<float>(kSurfaceWidth), kTitlebarHeight, kCornerRadius,
        kTitleFontSize, chromeStyle);
    window.configureCsdTitlebar(
        kTitlebarHeight,
        titlebarLayout.controlLeft,
        titlebarLayout.controlTop,
        chromeStyle.controlSize,
        chromeStyle.controlGap);

    // Keep the terminal's alpha background on the inexpensive rectangular
    // raster path.  The compositor owns the final rounded window mask.
    auto root = std::make_unique<lcl::ui::Container>();
    root->setBackgroundColor({17, 19, 23, 184});
    root->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
    root->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

    // This transparent, non-interactive child owns only the backdrop contract.
    // It must remain separate from the painted background: a full-window
    // software rounded-rect rasterization on each PTY update is prohibitively
    // expensive during typing and live resize.
    auto backdrop = std::make_unique<lcl::ui::BackdropSurface>();
    lcl::ui::BackdropSurface* backdropPtr = backdrop.get();
    backdrop->setInteractive(false);
    backdrop->setBorderRadius(kCornerRadius);
    backdrop->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    backdrop->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    backdrop->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    backdrop->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
    backdrop->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

    lcl::protocol::FilterOp blur{};
    blur.type = lcl::protocol::FilterType::Blur;
    blur.value = 3.5f;
    lcl::protocol::FilterOp glass{};
    glass.type = lcl::protocol::FilterType::Glass;
    glass.value = 1.0f;
    glass.profile = static_cast<uint8_t>(lcl::protocol::GlassProfile::Auto);
    glass.params[0] = 30.0f;
    glass.params[1] = 3.0f;
    glass.params[2] = 12.0f;
    backdrop->setFilters({blur, glass});

    auto terminalView = std::make_unique<lcl::apps::TerminalView>(terminal, kTitlebarHeight);
    lcl::apps::TerminalView* terminalViewPtr = terminalView.get();
    terminalView->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    terminalView->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    terminalView->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    terminalView->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
    terminalView->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

    auto titlebar = lcl::ui::chrome::buildWindowTitlebar(
        static_cast<float>(kSurfaceWidth),
        kTitlebarHeight,
        kCornerRadius,
        "LCL Terminal",
        kTitleFontSize,
        chromeStyle);
    lcl::ui::Container* titlebarPtr = titlebar.get();

    root->addChild(std::move(backdrop));
    root->addChild(std::move(terminalView));
    root->addChild(std::move(titlebar));
    window.setRootWidget(std::move(root));

    const auto updateTerminalGeometry = [&](uint32_t width, uint32_t height) {
        backdropPtr->getYogaNode().setWidth(static_cast<float>(width));
        backdropPtr->getYogaNode().setHeight(static_cast<float>(height));
        terminalViewPtr->getYogaNode().setWidth(static_cast<float>(width));
        terminalViewPtr->getYogaNode().setHeight(static_cast<float>(height));
        titlebarPtr->getYogaNode().setWidth(static_cast<float>(width));
        terminal.resize(static_cast<int>(width), std::max(1, static_cast<int>(height - kTitlebarHeight)));
        terminalViewPtr->markDirty();
    };
    updateTerminalGeometry(kSurfaceWidth, kSurfaceHeight);
    window.setOnResize(updateTerminalGeometry);
    window.setResizeTransform([](uint32_t requestedWidth, uint32_t requestedHeight) {
        int contentWidth = static_cast<int>(requestedWidth);
        int contentHeight = std::max(1, static_cast<int>(requestedHeight - kTitlebarHeight));
        lcl::apps::TerminalApp::getSnappedDimensions(
            contentWidth, contentHeight, contentWidth, contentHeight);
        return std::pair<uint32_t, uint32_t>{
            static_cast<uint32_t>(contentWidth),
            static_cast<uint32_t>(contentHeight + kTitlebarHeight),
        };
    });

    window.setOnRawKeyEvent([&](const lcl::ui::KeyEvent& event) {
        if (event.type != lcl::ui::KeyEventType::KeyDown ||
            !isTerminalControlKey(event.keyCode, event.modifiers)) {
            return false;
        }
        terminal.handleKey(static_cast<uint32_t>(event.keyCode), true, event.modifiers, event.codepoint);
        terminalViewPtr->markDirty();
        return true;
    });
    window.setOnRawTextInputEvent([&](const lcl::ui::TextInputEvent& event) {
        if (event.text.empty()) return true;
        const unsigned char firstByte = static_cast<unsigned char>(event.text.front());
        // Key-down owns terminal controls; text input owns printable UTF-8 only.
        if (firstByte < 32 || firstByte == 127) return true;
        terminal.handleText(event.text);
        terminalViewPtr->markDirty();
        return true;
    });
    window.setOnFrame([&] {
        const bool outputChanged = terminal.update();
        const bool cursorChanged = terminalViewPtr->updateCursorBlink();
        if (outputChanged || cursorChanged) {
            terminalViewPtr->markDirty();
        }
        if (!terminal.isAlive()) {
            window.requestQuit();
        }
    });

    if (!window.connectCompositor()) {
        terminal.shutdown();
        return 1;
    }

    window.runEventLoop();

    terminal.shutdown();
    return 0;
}
