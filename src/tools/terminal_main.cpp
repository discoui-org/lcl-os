#include <cmath>
#include <iostream>
#include <memory>

#include "apps/terminal/terminal_app.hpp"
#include "apps/terminal/terminal_view.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "platform/common/keyboard_types.hpp"
#include "render/raster_canvas.hpp"

namespace {

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

  lcl::ui::WindowApp window(lcl::render::makeRasterCanvas(), kSurfaceWidth,
                            kSurfaceHeight, "LCL Terminal");
  window.setAppId("org.lcl.terminal");
  window.setInitialBounds(80, 60, kSurfaceWidth, kSurfaceHeight);
  window.setResizePresentationMode(
      lcl::protocol::LCLResizePresentationMode::Live);
  window.setDecorationMode(lcl::protocol::LCLDecorationMode::SSD);
  window.setEdgeToEdge(true);
  window.setWindowCornerStyle(kCornerRadius, 2.0f);

  // The compositor owns the SSD chrome and final rounded window mask. The
  // client surface contains only terminal content.
  auto root = std::make_unique<lcl::ui::Container>();
  root->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
  root->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

  // One outer-surface effect chain supplies the same material behind both the
  // compositor-owned titlebar and the client content.
  auto backdrop = std::make_unique<lcl::ui::BackdropSurface>();
  lcl::ui::BackdropSurface *backdropPtr = backdrop.get();
  backdrop->setInteractive(false);
  backdrop->setEffectBounds(lcl::ui::EffectBounds::OuterSurface);
  backdrop->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  backdrop->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
  backdrop->getYogaNode().setPosition(YGEdgeTop, 0.0f);
  backdrop->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
  backdrop->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

  lcl::protocol::FilterOp blur{};
  blur.type = lcl::protocol::FilterType::Blur;
  blur.value = 50.0f;
  backdrop->setFilters({blur});
  backdrop->setTint({17, 19, 23, 184});

  auto terminalView = std::make_unique<lcl::apps::TerminalView>(terminal);
  lcl::apps::TerminalView *terminalViewPtr = terminalView.get();
  terminalView->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  terminalView->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
  terminalView->getYogaNode().setPosition(YGEdgeTop, 0.0f);
  terminalView->getYogaNode().setWidth(static_cast<float>(kSurfaceWidth));
  terminalView->getYogaNode().setHeight(static_cast<float>(kSurfaceHeight));

  root->addChild(std::move(backdrop));
  root->addChild(std::move(terminalView));
  window.setRootWidget(std::move(root));

  const auto updateTerminalGeometry = [&](uint32_t width, uint32_t height) {
    backdropPtr->getYogaNode().setWidth(static_cast<float>(width));
    backdropPtr->getYogaNode().setHeight(static_cast<float>(height));
    terminalViewPtr->getYogaNode().setWidth(static_cast<float>(width));
    terminalViewPtr->getYogaNode().setHeight(static_cast<float>(height));
    terminal.resize(static_cast<int>(width), static_cast<int>(height));
    terminalViewPtr->markDirty();
  };
  updateTerminalGeometry(kSurfaceWidth, kSurfaceHeight);
  window.setOnResize(updateTerminalGeometry);
  window.setResizeTransform([](float requestedWidth,
                               float requestedHeight,
                               lcl::protocol::LCLConfigureResizeReason reason) {
    if (reason != lcl::protocol::LCLConfigureResizeReason::Interactive) {
      return std::pair<float, float>{requestedWidth, requestedHeight};
    }
    int contentWidth = static_cast<int>(std::floor(requestedWidth));
    int contentHeight = static_cast<int>(std::floor(requestedHeight));
    lcl::apps::TerminalApp::getSnappedDimensions(contentWidth, contentHeight,
                                                 contentWidth, contentHeight);
    return std::pair<float, float>{
        static_cast<float>(contentWidth),
        static_cast<float>(contentHeight),
    };
  });

  window.setOnRawKeyEvent([&](const lcl::ui::KeyEvent &event) {
    if (event.type != lcl::ui::KeyEventType::KeyDown ||
        !isTerminalControlKey(event.key, event.modifiers)) {
      return false;
    }
    terminal.handleKey(event.key, true, event.modifiers, event.codepoint);
    terminalViewPtr->markDirty();
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
