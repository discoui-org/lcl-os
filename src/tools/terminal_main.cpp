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
#include "lcl-ui/widgets/text.hpp"

namespace {

constexpr uint32_t kSurfaceWidth = 540;
constexpr uint32_t kSurfaceHeight = 360;
constexpr float kTitlebarHeight = 34.0f;
constexpr float kCornerRadius = 20.0f;
constexpr float kControlSize = 16.0f;
constexpr float kControlGap = 6.0f;
constexpr float kControlLeft = 12.0f;
constexpr float kControlTop = 8.0f;

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

std::unique_ptr<lcl::ui::Container> makeTitlebar(float width) {
    auto titlebar = std::make_unique<lcl::ui::Container>();
    titlebar->setBackgroundColor({255, 255, 255, 0});
    titlebar->setBorderRadius(0.0f);
    titlebar->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    titlebar->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
    titlebar->getYogaNode().setPosition(YGEdgeTop, 0.0f);
    titlebar->getYogaNode().setWidth(width);
    titlebar->getYogaNode().setHeight(kTitlebarHeight);

    const float titleLeft = kControlLeft + (kControlSize * 3.0f) +
                            (kControlGap * 2.0f) + 12.0f;
    const auto makeControl = [](float left, const char* glyph) {
        auto control = std::make_unique<lcl::ui::Container>();
        control->setBackgroundColor({235, 241, 248, 56});
        control->setBorderColor({230, 238, 248, 120});
        control->setBorderWidth(1.0f);
        control->setBorderRadius(kControlSize * 0.5f);
        control->setBorderRoundness(2.0f);
        control->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        control->getYogaNode().setPosition(YGEdgeLeft, left);
        control->getYogaNode().setPosition(YGEdgeTop, kControlTop);
        control->getYogaNode().setWidth(kControlSize);
        control->getYogaNode().setHeight(kControlSize);

        auto icon = std::make_unique<lcl::ui::Text>(glyph);
        icon->setTextColor({236, 244, 252, 224});
        icon->setFontSize(11.0f);
        icon->getYogaNode().setPositionType(YGPositionTypeAbsolute);
        icon->getYogaNode().setPosition(YGEdgeLeft, kControlSize * 0.32f);
        icon->getYogaNode().setPosition(YGEdgeTop, kControlSize * 0.16f);
        control->addChild(std::move(icon));
        return control;
    };

    titlebar->addChild(makeControl(kControlLeft, "x"));
    titlebar->addChild(makeControl(kControlLeft + kControlSize + kControlGap, "-"));
    titlebar->addChild(makeControl(kControlLeft + (kControlSize + kControlGap) * 2.0f, "+"));

    auto title = std::make_unique<lcl::ui::Text>("LCL Terminal");
    title->setTextColor({240, 248, 255, 245});
    title->setFontSize(14.0f);
    title->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    title->getYogaNode().setPosition(YGEdgeLeft, titleLeft);
    title->getYogaNode().setPosition(YGEdgeTop, 9.0f);
    titlebar->addChild(std::move(title));

    return titlebar;
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
    window.configureCsdTitlebar(kTitlebarHeight, kControlLeft, kControlTop, kControlSize);

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

    auto titlebar = makeTitlebar(static_cast<float>(kSurfaceWidth));
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
