#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <linux/input-event-codes.h>

#include "apps/terminal/terminal_app.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "render/font_renderer.hpp"
#include "theme/palette.hpp"

// Render terminal app PTY lines into the SHM pixel buffer
static void renderTerminalFrame(uint32_t *shmPixels, int width, int height,
                                const lcl::apps::TerminalApp &app,
                                lcl::render::FontRenderer &fontRenderer,
                                int topInset) {
  if (!shmPixels || width <= 0 || height <= 0)
    return;

  // Dark neutral glass-tinted background for libadwaita/macOS hybrid theme.
  const uint32_t kBgColor = 0xB8111317;
  const uint32_t kTextColor = 0xFFECEFF4;
  const uint32_t kCursorColor = 0xFFC4CAD3;

  std::fill(shmPixels, shmPixels + (width * height), kBgColor);

  auto content = app.getRenderContent();
  int fontCellW = fontRenderer.getCellWidth();
  int fontCellH = fontRenderer.getCellHeight();
  int lineSpacing = fontCellH + 2;
  int pad = 8;
  int padTop = std::max(pad, topInset + pad);

  int maxRows = std::max(1, (height - padTop - pad) / lineSpacing);

  // Auto-scroll: render only the latest maxRows lines
  int startLine = std::max(0, static_cast<int>(content.lines.size()) - maxRows);
  int curY = padTop;

  for (size_t l = startLine;
       l < content.lines.size() && curY + fontCellH <= height - pad; ++l) {
    const auto &line = content.lines[l];
    if (!line.empty()) {
      fontRenderer.renderStringClipped(shmPixels, width, height, pad, curY,
                                       line, kTextColor, pad, padTop,
                                       width - pad, height - pad);
    }
    curY += lineSpacing;
  }

  // Draw active cursor at write head
  int lastLineY = curY - lineSpacing;
  if (lastLineY < padTop)
    lastLineY = padTop;

  std::string lastLineText = content.lines.empty() ? "" : content.lines.back();
  int caretCol = content.cursorCol;
  std::string caretPrefix =
      (caretCol >= 0 && caretCol <= static_cast<int>(lastLineText.size()))
          ? lastLineText.substr(0, caretCol)
          : lastLineText;

  int caretX = pad + fontRenderer.getTextWidth(caretPrefix);

  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  bool showCursor = content.forceCursorSolid || ((ms / 500) % 2 == 0);

  if (showCursor && caretX + fontCellW <= width - pad &&
      lastLineY + fontCellH <= height - pad) {
    for (int r = 0; r < fontCellH; ++r) {
      int py = lastLineY + r;
      if (py >= 0 && py < height) {
        for (int c = 0; c < fontCellW; ++c) {
          int px = caretX + c;
          if (px >= 0 && px < width) {
            shmPixels[py * width + px] = kCursorColor;
          }
        }
      }
    }
  }
}

static void applyRoundedCornerClip(uint32_t *pixels, int width, int height,
                                   int radiusPx) {
  if (!pixels || width <= 0 || height <= 0 || radiusPx <= 0)
    return;

  const int r = std::max(1, std::min(radiusPx, std::min(width, height) / 2));
  const int rr = r * r;

  for (int y = 0; y < r; ++y) {
    for (int x = 0; x < r; ++x) {
      const int dx = r - x;
      const int dy = r - y;
      if ((dx * dx + dy * dy) <= rr)
        continue;

      pixels[y * width + x] = 0x00000000u;
      pixels[y * width + (width - 1 - x)] = 0x00000000u;
      pixels[(height - 1 - y) * width + x] = 0x00000000u;
      pixels[(height - 1 - y) * width + (width - 1 - x)] = 0x00000000u;
    }
  }
}

int main() {
  std::cout << "====================================================\n"
            << "  lcl-terminal v0.1.0 - Standalone Client App      \n"
            << "====================================================\n";

  // 1. Connect to Compositor Unix Domain Socket
  int socketFd = -1;
  for (int i = 0; i < 50; ++i) {
    socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFd >= 0) {
      struct sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::strncpy(addr.sun_path, lcl::core::kCompositorSocket,
                   sizeof(addr.sun_path) - 1);
      if (connect(socketFd, reinterpret_cast<struct sockaddr *>(&addr),
                  sizeof(addr)) == 0) {
        break;
      }
      close(socketFd);
      socketFd = -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (socketFd < 0) {
    std::cerr << "[LCL Terminal ERROR] Could not connect to compositor socket: "
              << lcl::core::kCompositorSocket << "\n";
    return 1;
  }

  std::cout
      << "[LCL Terminal] Connected to Compositor IPC socket successfully.\n";

  // 2. Register role as CLIENT_APP
  lcl::protocol::LCLHeader regHeader{};
  regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
  regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

  lcl::protocol::LCLMsgRegisterRole regMsg{};
  regMsg.role = lcl::protocol::LCLRole::ClientApp;
  std::strncpy(regMsg.clientName, "lcl-terminal",
               sizeof(regMsg.clientName) - 1);

  lcl::protocol::sendMsgWithFd(socketFd, regHeader, &regMsg);
  std::cout << "[LCL Terminal] Registered as CLIENT_APP role on lcl-core "
               "compositor.\n";

  // 3. Request Surface Creation
  const int kSurfW = 540;
  const int kSurfH = 360;
  const int kClientTitleBarH = 34;
  const int kTerminalCornerRadiusPx = 20;

  lcl::protocol::LCLHeader surfHeader{};
  surfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
  surfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

  lcl::protocol::LCLMsgSurfaceCreate surfMsg{};
  surfMsg.surfaceId = 1;
  surfMsg.x = 80;
  surfMsg.y = 60;
  surfMsg.width = kSurfW;
  surfMsg.height = kSurfH;
  std::strncpy(surfMsg.title, "LCL Terminal", sizeof(surfMsg.title) - 1);

  lcl::protocol::sendMsgWithFd(socketFd, surfHeader, &surfMsg);
  std::cout << "[LCL Terminal] Requested surface creation (ID: 1, " << kSurfW
            << "x" << kSurfH << ") from compositor.\n";

  // Use client-side titlebar rendering (terminal draws its own transparent bar).
  lcl::protocol::LCLHeader decHeader{};
  decHeader.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
  decHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);
  lcl::protocol::LCLMsgSetDecorationMode decMsg{};
  decMsg.surfaceId = 1;
  decMsg.mode = lcl::protocol::LCLDecorationMode::None;
  lcl::protocol::sendMsgWithFd(socketFd, decHeader, &decMsg);

  // Set Effect Graph for Surface 1 (Terminal)
  // Region: full content surface, Source: Backdrop
  lcl::protocol::FilterOp terminalBlur{};
  terminalBlur.type = lcl::protocol::FilterType::Blur;
  terminalBlur.value = 3.5f;

  lcl::protocol::FilterOp terminalGlass{};
  terminalGlass.type = lcl::protocol::FilterType::Glass;
  terminalGlass.value = 1.0f; // reserved for legacy strength; ignored by glass shader
  terminalGlass.profile = static_cast<uint8_t>(lcl::protocol::GlassProfile::Auto); // reserved
  terminalGlass.params[0] = 30.0f; // thickness (pixels)
  terminalGlass.params[1] = 3.0f; // refraction factor
  terminalGlass.params[2] = 12.0f; // dispersion gain

  std::vector<lcl::protocol::FilterOp> termFilters = {terminalBlur,
                                                      terminalGlass};

  auto sendTerminalEffectGraph = [&](int width, int height) {
    lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
    graphMsg.surfaceId = 1;
    graphMsg.regionCount = 1;
    graphMsg.filterCount = static_cast<uint32_t>(termFilters.size());

    lcl::protocol::EffectRegion graphRegion{};
    graphRegion.x = 0;
    graphRegion.y = 0;
    graphRegion.width = static_cast<uint32_t>(width);
    graphRegion.height = static_cast<uint32_t>(height);
    graphRegion.source = lcl::protocol::EffectSourceType::Backdrop;
    graphRegion.blendMode = lcl::protocol::EffectBlendMode::Normal;
    graphRegion.cornerRadius = static_cast<float>(kTerminalCornerRadiusPx);
    graphRegion.filterCount = static_cast<uint16_t>(termFilters.size());
    graphRegion.filterOffset = 0;
    graphRegion.opacity = 1.0f;

    size_t graphPayloadSize = sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader) +
                              sizeof(lcl::protocol::EffectRegion) +
                              termFilters.size() * sizeof(lcl::protocol::FilterOp);
    std::vector<uint8_t> graphPayload(graphPayloadSize);
    uint8_t* graphDst = graphPayload.data();
    std::memcpy(graphDst, &graphMsg, sizeof(graphMsg));
    graphDst += sizeof(graphMsg);
    std::memcpy(graphDst, &graphRegion, sizeof(graphRegion));
    graphDst += sizeof(graphRegion);
    std::memcpy(graphDst, termFilters.data(), termFilters.size() * sizeof(lcl::protocol::FilterOp));

    lcl::protocol::LCLHeader graphHeader{};
    graphHeader.opcode = lcl::protocol::LCLOpcode::SetEffectGraph;
    graphHeader.payloadSize = static_cast<uint32_t>(graphPayload.size());
    lcl::protocol::sendMsgWithFd(socketFd, graphHeader, graphPayload.data());
  };

  sendTerminalEffectGraph(kSurfW, kSurfH);
    std::cout << "[LCL Terminal] Set effect graph (blur + strong glass) for surface 1.\n";

  // 4. Create Shared Memory (memfd) Framebuffer for Surface 1
  size_t shmSize = kSurfW * kSurfH * 4;
  int shmFd = memfd_create("lcl_terminal_shm", MFD_CLOEXEC);
  uint32_t *shmPixels = nullptr;
  if (shmFd >= 0) {
    ftruncate(shmFd, shmSize);
    std::cout << "[LCL Terminal] Allocated Shared Memory Buffer (memfd: "
              << shmFd << ", " << shmSize << " bytes).\n";

    shmPixels = reinterpret_cast<uint32_t *>(
        mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));
    if (shmPixels == MAP_FAILED) {
      shmPixels = nullptr;
      std::cerr << "[LCL Terminal ERROR] Failed to mmap own SHM buffer.\n";
    }
  }

  // 5. Initialize TrueType Vector Font Engine
  lcl::render::FontRenderer fontRenderer;
  std::vector<std::string> fontPaths = {
      "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
      "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"};
  for (const auto &path : fontPaths) {
    if (fontRenderer.loadFont(path, 15.0f)) {
      std::cout << "[LCL Terminal] Loaded TTF font: " << path << "\n";
      break;
    }
  }
  if (!fontRenderer.isInitialized()) {
    std::cerr << "[LCL Terminal ERROR] Could not load terminal font.\n";
    if (shmPixels)
      munmap(shmPixels, shmSize);
    if (shmFd >= 0)
      close(shmFd);
    close(socketFd);
    return 1;
  }

  // 6. Initialize local PTY and terminal shell instance
  lcl::apps::TerminalApp app;
  if (!app.initialize(1)) {
    std::cerr
        << "[LCL Terminal ERROR] Terminal App PTY initialization failed.\n";
    if (shmPixels)
      munmap(shmPixels, shmSize);
    if (shmFd >= 0)
      close(shmFd);
    close(socketFd);
    return 1;
  }

  // Set socket non-blocking for IPC input polling
  int flags = fcntl(socketFd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
  }

  // Build client-side titlebar widgets directly inside terminal content surface.
  auto titlebarRoot = std::make_unique<lcl::ui::Container>();
  titlebarRoot->setBackgroundColor(lcl::ui::Color{0, 0, 0, 0});
  titlebarRoot->setBorderRadius(0.0f);
  titlebarRoot->getYogaNode().setWidth(static_cast<float>(kSurfW));
  titlebarRoot->getYogaNode().setHeight(static_cast<float>(kSurfH));

  lcl::ui::Container *titleBarWidget = nullptr;

  auto titleBar = std::make_unique<lcl::ui::Container>();
  titleBarWidget = titleBar.get();
  titleBar->setBackgroundColor(lcl::ui::Color{255, 255, 255, 0});
  titleBar->setBorderRadius(0.0f);
  titleBar->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  titleBar->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
  titleBar->getYogaNode().setPosition(YGEdgeTop, 0.0f);
  titleBar->getYogaNode().setWidth(static_cast<float>(kSurfW));
  titleBar->getYogaNode().setHeight(static_cast<float>(kClientTitleBarH));

  const float ctrlSize = 16.0f;
  const float ctrlGap = 6.0f;
  const float ctrlLeft = std::max(8.0f, static_cast<float>(kTerminalCornerRadiusPx) - 8.0f);
  const float titleLeft = ctrlLeft + (ctrlSize * 3.0f) + (ctrlGap * 2.0f) + 12.0f;

  auto titleText = std::make_unique<lcl::ui::Text>("LCL Terminal");
  titleText->setTextColor(lcl::ui::Color{240, 248, 255, 245});
  titleText->setFontSize(14.0f);
  titleText->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  titleText->getYogaNode().setPosition(YGEdgeLeft, titleLeft);
  titleText->getYogaNode().setPosition(YGEdgeTop, 9.0f);
  titleBar->addChild(std::move(titleText));

  titlebarRoot->addChild(std::move(titleBar));

  lcl::ui::WindowApp titlebarApp(kSurfW, kSurfH, "LCL Terminal Titlebar");
  titlebarApp.setRootWidget(std::move(titlebarRoot));

  int curW = kSurfW;
  int curH = kSurfH;
  std::vector<uint32_t> localPixels(curW * curH, 0xB8111317);

  auto drawTitlebarWidgets = [&](int frameW, int frameH) {
    auto &titlebarRenderer = titlebarApp.getRenderer();
    titlebarRenderer.setTargetPixels(localPixels.data(),
                                     static_cast<uint32_t>(frameW),
                                     static_cast<uint32_t>(frameH));
    if (auto *root = titlebarApp.getRootWidget()) {
      root->getYogaNode().setWidth(static_cast<float>(frameW));
      root->getYogaNode().setHeight(static_cast<float>(frameH));
    }
    if (titleBarWidget) {
      titleBarWidget->getYogaNode().setWidth(static_cast<float>(frameW));
    }
    if (auto *root = titlebarApp.getRootWidget()) {
      root->getYogaNode().calculateLayout(static_cast<float>(frameW),
                                          static_cast<float>(frameH));
      root->syncLayout(0.0f, 0.0f);
      lcl::ui::Rect damage{0.0f, 0.0f, static_cast<float>(frameW),
                           static_cast<float>(kClientTitleBarH)};
      root->draw(reinterpret_cast<SkCanvas*>(&titlebarRenderer), damage);
    }
  };

  // Send initial ATTACH_BUFFER
  app.resize(kSurfW, std::max(1, kSurfH - kClientTitleBarH));
  renderTerminalFrame(localPixels.data(), kSurfW, kSurfH, app, fontRenderer,
                      kClientTitleBarH);
  drawTitlebarWidgets(kSurfW, kSurfH);
  applyRoundedCornerClip(localPixels.data(), kSurfW, kSurfH,
                         kTerminalCornerRadiusPx);
  if (shmPixels) {
    std::memcpy(shmPixels, localPixels.data(), shmSize);
  }

  lcl::protocol::LCLHeader attachHeader{};
  attachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
  attachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

  lcl::protocol::LCLMsgAttachBuffer attachMsg{};
  attachMsg.surfaceId = 1;
  attachMsg.width = kSurfW;
  attachMsg.height = kSurfH;
  attachMsg.stride = kSurfW * 4;
  attachMsg.format = 1; // ARGB8888

  lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, shmFd);
  std::cout << "[LCL Terminal] Sent initial ATTACH_BUFFER (memfd: " << shmFd
            << ") for Surface 1.\n";
  std::cout << "[LCL Terminal] Standalone process running (PID: " << getpid()
            << "). PTY shell active.\n";

  // 7. Event loop: poll IPC keypresses, PTY output & update SHM frame
  auto lastBlink = std::chrono::steady_clock::now();

  while (app.isAlive()) {
    bool updated = app.update();

    int targetW = -1;
    int targetH = -1;

    // Drain pending IPC input events & resize ConfigureBounds from Compositor
    while (true) {
      lcl::protocol::LCLHeader header{};
      std::vector<uint8_t> payload;
      int receivedFd = -1;
      (void)receivedFd;
      if (lcl::protocol::recvMsgWithFd(socketFd, header, payload, receivedFd)) {
        if (header.opcode == lcl::protocol::LCLOpcode::InputEvent &&
            payload.size() >= sizeof(lcl::protocol::LCLMsgInputEvent)) {
          auto *inputMsg =
              reinterpret_cast<const lcl::protocol::LCLMsgInputEvent *>(
                  payload.data());
          if (inputMsg->type == 1) { // KeyDown
            // Text characters must come only from type=5 to avoid double input.
            // Keep KeyDown path only for control/navigation keys that do not
            // reliably emit a text codepoint event.
            switch (inputMsg->key) {
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
                app.handleKey(inputMsg->key, true, inputMsg->modifiers, 0);
                updated = true;
                break;
              default:
                break;
            }
          } else if (inputMsg->type == 2) { // KeyUp
            // Terminal input pipeline is press/text-driven; key-up is ignored.
          } else if (inputMsg->type == 5) { // KeyPress / TextInput
            // Accept only printable text here. Control keys (Backspace, Enter,
            // arrows, etc.) are handled only in KeyDown to avoid duplicates.
            if (inputMsg->codepoint >= 32 && inputMsg->codepoint != 127) {
              app.handleKey(0, true, inputMsg->modifiers,
                            static_cast<char32_t>(inputMsg->codepoint));
              updated = true;
            }
          }
        } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                   payload.size() >=
                       sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
          auto *cfg =
              reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds *>(
                  payload.data());
            const int reqW = static_cast<int>(cfg->width);
            const int reqH = static_cast<int>(cfg->height);

          int contentW = reqW;
          int contentH = std::max(1, reqH - kClientTitleBarH);
            lcl::apps::TerminalApp::getSnappedDimensions(contentW, contentH,
                                   contentW, contentH);
          int newW = contentW;
          int newH = contentH + kClientTitleBarH;

          if (newW > 0 && newH > 0) {
            targetW = newW;
            targetH = newH;
          }
        }
      } else {
        break;
      }
    }

    // Event Coalescing: Execute resize ONCE for the most recent dimensions in
    // socket queue
    if (targetW > 0 && targetH > 0 && (targetW != curW || targetH != curH)) {
      curW = targetW;
      curH = targetH;

      if (shmPixels) {
        munmap(shmPixels, shmSize);
        shmPixels = nullptr;
      }
      if (shmFd >= 0) {
        close(shmFd);
        shmFd = -1;
      }

      shmSize = curW * curH * 4;
      localPixels.resize(curW * curH, 0xB8111317);

      shmFd = memfd_create("lcl_term_shm", MFD_CLOEXEC);
      if (shmFd >= 0) {
        ftruncate(shmFd, shmSize);
        shmPixels = reinterpret_cast<uint32_t *>(mmap(
            nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));

        if (shmPixels == MAP_FAILED) {
          shmPixels = nullptr;
        } else {
          std::fill_n(shmPixels, curW * curH, 0xB8111317);
        }
      }

      app.resize(curW, std::max(1, curH - kClientTitleBarH));
      // Keep effect region in sync with current surface size.
        sendTerminalEffectGraph(curW, curH);

      // Pre-render terminal layout for new dimensions & copy to shmPixels
      // BEFORE committing to Compositor!
      if (shmPixels) {
        renderTerminalFrame(localPixels.data(), curW, curH, app, fontRenderer,
                            kClientTitleBarH);
        drawTitlebarWidgets(curW, curH);
        applyRoundedCornerClip(localPixels.data(), curW, curH,
                               kTerminalCornerRadiusPx);
        std::memcpy(shmPixels, localPixels.data(), shmSize);
      }

      attachMsg.width = curW;
      attachMsg.height = curH;
      attachMsg.stride = curW * 4;

      int passFd = shmFd;
      lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, passFd);
      updated = false;
    }

    auto now = std::chrono::steady_clock::now();
    auto blinkMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBlink)
            .count();
    if (blinkMs >= 500) {
      lastBlink = now;
      updated = true;
    }

    if (updated && shmPixels) {
      renderTerminalFrame(localPixels.data(), curW, curH, app, fontRenderer,
                          kClientTitleBarH);
      drawTitlebarWidgets(curW, curH);
      applyRoundedCornerClip(localPixels.data(), curW, curH,
                 kTerminalCornerRadiusPx);
      std::memcpy(shmPixels, localPixels.data(), shmSize);

      // Re-notify compositor of buffer redraw
      lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, -1);
    }

    std::this_thread::sleep_for(std::chrono::microseconds(6900));
  }

  app.shutdown();
  if (shmPixels)
    munmap(shmPixels, shmSize);
  if (shmFd >= 0)
    close(shmFd);
  close(socketFd);
  return 0;
}
