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

#include "apps/terminal/terminal_app.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/core/window_app.hpp"
#include "render/font_renderer.hpp"

// Render terminal app PTY lines into the SHM pixel buffer
static void renderTerminalFrame(uint32_t *shmPixels, int width, int height,
                                const lcl::apps::TerminalApp &app,
                                lcl::render::FontRenderer &fontRenderer) {
  if (!shmPixels || width <= 0 || height <= 0)
    return;

  // Translucent dark terminal background color (ARGB - ~85% alpha dark slate)
  const uint32_t kBgColor = 0x00000000;
  const uint32_t kTextColor = 0xFF38BDF8;   // Electric Cyan text
  const uint32_t kCursorColor = 0xFF00FF88; // Bright Green cursor block

  std::fill(shmPixels, shmPixels + (width * height), kBgColor);

  auto content = app.getRenderContent();
    int fontCellW = fontRenderer.getCellWidth();
    int fontCellH = fontRenderer.getCellHeight();
  int lineSpacing = fontCellH + 2;
  int pad = 8;

  int maxRows = std::max(1, (height - 2 * pad) / lineSpacing);

  // Auto-scroll: render only the latest maxRows lines
  int startLine = std::max(0, static_cast<int>(content.lines.size()) - maxRows);
  int curY = pad;

  for (size_t l = startLine;
       l < content.lines.size() && curY + fontCellH <= height - pad; ++l) {
    const auto &line = content.lines[l];
    if (!line.empty()) {
      fontRenderer.renderStringClipped(shmPixels, width, height, pad, curY,
                                       line, kTextColor, pad, pad,
                                       width - pad, height - pad);
    }
    curY += lineSpacing;
  }

  // Draw active cursor at write head
  int lastLineY = curY - lineSpacing;
  if (lastLineY < pad)
    lastLineY = pad;

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

    // Set Effect Graph for Surface 1 (Terminal)
    // Region: full content surface, Source: Backdrop
    // Refraction test: glass filter with thickness/refraction/dispersion only.
  lcl::protocol::FilterOp terminalGlass{};
  terminalGlass.type = lcl::protocol::FilterType::Glass;
  terminalGlass.value = 1.0f; // reserved for legacy strength; ignored by glass shader
  terminalGlass.profile = static_cast<uint8_t>(lcl::protocol::GlassProfile::Auto); // reserved
  terminalGlass.params[0] = 20.0f; // thickness (pixels)
  terminalGlass.params[1] = 1.40f; // refraction factor
  terminalGlass.params[2] = 7.0f;  // dispersion gain

  std::vector<lcl::protocol::FilterOp> termFilters = {terminalGlass};

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
    std::cout << "[LCL Terminal] Set effect graph (glass: thickness/refraction/dispersion) for surface 1.\n";

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

  // Initialize WindowApp & direct raw keypress hook for PTY input routing
  lcl::ui::WindowApp windowApp(kSurfW, kSurfH, "LCL Terminal");
  windowApp.setOnRawKeyEvent([&app](const lcl::ui::KeyEvent &ev) {
    app.handleKey(ev.keyCode, ev.type == lcl::ui::KeyEventType::KeyDown,
                  ev.modifiers, ev.codepoint);
    return true; // Intercept & consume directly for PTY shell
  });

  int curW = kSurfW;
  int curH = kSurfH;
  bool termNeedsAttach = true;
  std::vector<uint32_t> localPixels(curW * curH, 0xD90F172A);

  // Send initial ATTACH_BUFFER
  renderTerminalFrame(localPixels.data(), kSurfW, kSurfH, app, fontRenderer);
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
  termNeedsAttach = false;
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
      if (lcl::protocol::recvMsgWithFd(socketFd, header, payload, receivedFd)) {
        if (header.opcode == lcl::protocol::LCLOpcode::InputEvent &&
            payload.size() >= sizeof(lcl::protocol::LCLMsgInputEvent)) {
          auto *inputMsg =
              reinterpret_cast<const lcl::protocol::LCLMsgInputEvent *>(
                  payload.data());
          if (inputMsg->type == 1) { // KeyDown
            windowApp.sendKeyDown(inputMsg->key,
                                  static_cast<char32_t>(inputMsg->codepoint),
                                  inputMsg->modifiers);
            updated = true;
          } else if (inputMsg->type == 2) { // KeyUp
            windowApp.sendKeyUp(inputMsg->key, inputMsg->modifiers);
            updated = true;
          } else if (inputMsg->type == 5) { // KeyPress / TextInput
            if (inputMsg->codepoint > 0) {
              std::string utf8;
              char32_t cp = inputMsg->codepoint;
              if (cp <= 0x7F) {
                utf8.push_back(static_cast<char>(cp));
              } else if (cp <= 0x7FF) {
                utf8.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              }
              windowApp.sendTextInput(utf8);
            }
            updated = true;
          }
        } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                   payload.size() >=
                       sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
          auto *cfg =
              reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds *>(
                  payload.data());
          int reqW = static_cast<int>(cfg->width);
          int reqH = static_cast<int>(cfg->height);

          int newW = reqW;
          int newH = reqH;
          lcl::apps::TerminalApp::getSnappedDimensions(reqW, reqH, newW, newH);

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
      localPixels.resize(curW * curH, 0xD90F172A);

      shmFd = memfd_create("lcl_term_shm", MFD_CLOEXEC);
      if (shmFd >= 0) {
        ftruncate(shmFd, shmSize);
        shmPixels = reinterpret_cast<uint32_t *>(mmap(
            nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));

        if (shmPixels == MAP_FAILED) {
          shmPixels = nullptr;
        } else {
          std::fill_n(shmPixels, curW * curH, 0xD90F172A);
        }
      }

      app.resize(curW, curH);
      windowApp.getRootWidget()->getYogaNode().setWidth(
          static_cast<float>(curW));
      windowApp.getRootWidget()->getYogaNode().setHeight(
          static_cast<float>(curH));

        // Keep effect region in sync with current surface size.
        sendTerminalEffectGraph(curW, curH);

      // Pre-render terminal layout for new dimensions & copy to shmPixels
      // BEFORE committing to Compositor!
      if (shmPixels) {
        renderTerminalFrame(localPixels.data(), curW, curH, app, fontRenderer);
        std::memcpy(shmPixels, localPixels.data(), shmSize);
      }

      attachMsg.width = curW;
      attachMsg.height = curH;
      attachMsg.stride = curW * 4;

      int passFd = shmFd;
      lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, passFd);
      termNeedsAttach = false;
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
      renderTerminalFrame(localPixels.data(), curW, curH, app, fontRenderer);
      std::memcpy(shmPixels, localPixels.data(), shmSize);

      // Re-notify compositor of buffer redraw
      int passFd = termNeedsAttach ? shmFd : -1;
      lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, passFd);
      termNeedsAttach = false;
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
