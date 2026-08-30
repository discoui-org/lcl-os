#include <iostream>
#include <memory>

#include "apps/terminal/terminal_app.hpp"
#include "apps/terminal/terminal_view.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "platforms/common/keyboard_types.hpp"
#include "system/render/raster_canvas.hpp"

namespace {

namespace layout = lcl::ui::layout;

constexpr uint32_t kSurfaceWidth = 540;
constexpr uint32_t kSurfaceHeight = 360;
constexpr float kCornerRadius = 20.0f;

bool isTerminalControlKey(lcl::platform::PhysicalKey key, uint8_t modifiers) {
  if ((modifiers & lcl::platform::kModCtrl) != 0) {
    return true;
  }

  switch (key) {
  case lcl::platform::PhysicalKey::Enter:
  case lcl::platform::PhysicalKey::KpEnter:
  case lcl::platform::PhysicalKey::Backspace:
  case lcl::platform::PhysicalKey::Tab:
  case lcl::platform::PhysicalKey::Escape:
  case lcl::platform::PhysicalKey::ArrowUp:
  case lcl::platform::PhysicalKey::ArrowDown:
  case lcl::platform::PhysicalKey::ArrowLeft:
  case lcl::platform::PhysicalKey::ArrowRight:
  case lcl::platform::PhysicalKey::Home:
  case lcl::platform::PhysicalKey::End:
  case lcl::platform::PhysicalKey::PageUp:
  case lcl::platform::PhysicalKey::PageDown:
  case lcl::platform::PhysicalKey::Insert:
  case lcl::platform::PhysicalKey::Delete:
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

  lcl::ui::WindowApp window(lcl::render::makeDisplayListCanvas(), kSurfaceWidth,
                            kSurfaceHeight, "LCL Terminal");
  window.setAppId("org.lcl.terminal");
  window.setInitialBounds(80, 60, kSurfaceWidth, kSurfaceHeight);
  if (!window.setResizeConstraints({16.0f, 16.0f, 8.0f, 16.0f})) {
    std::cerr << "[LCL Terminal ERROR] Invalid resize constraints.\n";
    return 1;
  }
  window.setDecorationMode(lcl::protocol::LCLDecorationMode::SSD);
  window.setEdgeToEdge(true);
  window.setWindowCornerStyle(kCornerRadius, 2.0f);

  // DesktopWM owns the attached frame; the compositor owns only the generic
  // WindowGroup shape. The client surface contains terminal content only.
  auto root = std::make_unique<lcl::ui::Container>();
  root->setWidth(static_cast<float>(kSurfaceWidth));
  root->setHeight(static_cast<float>(kSurfaceHeight));

  // One outer-surface effect chain supplies the same material behind both the
  // DesktopWM titlebar and the client content.
  auto backdrop = std::make_unique<lcl::ui::BackdropSurface>();
  lcl::ui::BackdropSurface *backdropPtr = backdrop.get();
  backdrop->setInteractive(false);
  backdrop->setEffectBounds(lcl::ui::EffectBounds::OuterSurface);
  backdrop->setPositionType(layout::PositionType::Absolute);
  backdrop->setPosition(layout::Edge::Left, 0.0f);
  backdrop->setPosition(layout::Edge::Top, 0.0f);
  backdrop->setWidth(static_cast<float>(kSurfaceWidth));
  backdrop->setHeight(static_cast<float>(kSurfaceHeight));

  backdrop->addFilter(lcl::protocol::FilterType::Blur, 50.0f);
  backdrop->addFilter(lcl::protocol::FilterType::Saturation, 2.0f);
  backdrop->addFilter(lcl::protocol::FilterType::Brightness, 1.1f);
  backdrop->setTint({17, 19, 23, 184});

  auto terminalView = std::make_unique<lcl::apps::TerminalView>(terminal);
  lcl::apps::TerminalView *terminalViewPtr = terminalView.get();
  terminalView->setPositionType(layout::PositionType::Absolute);
  terminalView->setPosition(layout::Edge::Left, 0.0f);
  terminalView->setPosition(layout::Edge::Top, 0.0f);
  terminalView->setWidth(static_cast<float>(kSurfaceWidth));
  terminalView->setHeight(static_cast<float>(kSurfaceHeight));

  root->addChild(std::move(backdrop));
  root->addChild(std::move(terminalView));
  window.setRootWidget(std::move(root));

  const auto updateTerminalGeometry = [&](uint32_t width, uint32_t height) {
    backdropPtr->setWidth(static_cast<float>(width));
    backdropPtr->setHeight(static_cast<float>(height));
    terminalViewPtr->setWidth(static_cast<float>(width));
    terminalViewPtr->setHeight(static_cast<float>(height));
    terminal.resize(static_cast<int>(width), static_cast<int>(height));
    terminalViewPtr->invalidatePaint();
  };
  updateTerminalGeometry(kSurfaceWidth, kSurfaceHeight);
  window.setOnResize(updateTerminalGeometry);
  window.setOnRawKeyEvent([&](const lcl::ui::KeyEvent &event) {
    if (event.type != lcl::ui::KeyEventType::KeyDown ||
        !isTerminalControlKey(event.key, event.modifiers)) {
      return false;
    }
    terminal.handleKey(event.key, true, event.modifiers, event.codepoint);
    terminalViewPtr->invalidatePaint();
    return true;
  });
  window.setOnRawTextInputEvent([&](const lcl::ui::TextInputEvent &event) {
    if (event.text.empty())
      return true;
    const unsigned char firstByte =
        static_cast<unsigned char>(event.text.front());
    // Key-down owns terminal controls; text input owns printable UTF-8 only.
    if (firstByte < 32 || firstByte == 127)
      return true;
    terminal.handleText(event.text);
    terminalViewPtr->invalidatePaint();
    return true;
  });
  window.setOnFrame([&] {
    const bool outputChanged = terminal.update();
    // TerminalView owns cursor-cell damage; PTY output changes the model and
    // therefore still invalidates the complete terminal content.
    terminalViewPtr->updateCursorBlink();
    if (outputChanged) {
      terminalViewPtr->invalidatePaint();
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
