#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cstring>

#include "apps/terminal/terminal_app.hpp"
#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "render/font_renderer.hpp"
#include "lcl-ui/core/window_app.hpp"

// Simple 8x16 VGA font glyph row mask helper for fallback rendering
static uint8_t getSimpleGlyphRow(char c, int row) {
    if (row < 0 || row >= 16) return 0;
    if (c >= '0' && c <= '9') {
        static const uint8_t digits[10][16] = {
            {0x00,0x00,0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0x66,0x3E,0x06,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
        };
        return digits[c - '0'][row];
    }
    if (c >= 'A' && c <= 'Z') {
        static const uint8_t upper[26][16] = {
            {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0xC0,0xC0,0xC0,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x66,0xC0,0xC0,0xCE,0xC6,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x62,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xDA,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x3E,0x66,0x60,0x3C,0x06,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
        };
        return upper[c - 'A'][row];
    }
    if (c >= 'a' && c <= 'z') {
        static const uint8_t lower[26][16] = {
            {0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3C,0x66,0x7E,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x06,0x00,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x5C,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x3E,0x60,0x3C,0x06,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0xC6,0xC6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x66,0x3C,0x18,0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            {0x00,0x00,0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}
        };
        return lower[c - 'a'][row];
    }
    switch (c) {
        case '/': return uint8_t("\x00\x00\x02\x06\x0C\x18\x30\x60\x40\x00\x00\x00\x00\x00\x00\x00"[row]);
        case ':': return uint8_t("\x00\x00\x00\x18\x18\x00\x18\x18\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '$': return uint8_t("\x00\x18\x3E\x68\x3C\x0B\x7C\x18\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '#': return uint8_t("\x00\x24\x24\x7E\x24\x7E\x24\x24\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '-': return uint8_t("\x00\x00\x00\x00\x7E\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '_': return uint8_t("\x00\x00\x00\x00\x00\x00\x00\x00\xFE\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '.': return uint8_t("\x00\x00\x00\x00\x00\x18\x18\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '@': return uint8_t("\x00\x00\x3C\x66\x6E\x6E\x6E\x60\x3E\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '~': return uint8_t("\x00\x00\x00\x00\x6C\x36\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '%': return uint8_t("\x00\x00\x62\x64\x08\x10\x26\x46\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        default:  return 0x00;
    }
}

static void drawFallbackString(uint32_t* shmPixels, int width, int height,
                               int startX, int startY, const std::string& text, uint32_t fgColor) {
    int curX = startX;
    int curY = startY;
    for (char c : text) {
        if (c == '\n') {
            curX = startX;
            curY += 18;
            continue;
        }
        for (int r = 0; r < 16; ++r) {
            uint8_t rowMask = getSimpleGlyphRow(c, r);
            int py = curY + r;
            if (py < 0 || py >= height) continue;
            for (int col = 0; col < 8; ++col) {
                if ((rowMask >> (7 - col)) & 1) {
                    int px = curX + col;
                    if (px >= 0 && px < width) {
                        shmPixels[py * width + px] = fgColor;
                    }
                }
            }
        }
        curX += 8;
    }
}

// Render terminal app PTY lines into the SHM pixel buffer
static void renderTerminalFrame(uint32_t* shmPixels, int width, int height,
                                const lcl::apps::TerminalApp& app,
                                lcl::render::FontRenderer& fontRenderer) {
    if (!shmPixels || width <= 0 || height <= 0) return;

    // Dark terminal background color (ARGB)
    const uint32_t kBgColor   = 0xFF14161D;
    const uint32_t kTextColor = 0xFF38BDF8;   // Electric Cyan text
    const uint32_t kCursorColor = 0xFF00FF88; // Bright Green cursor block

    std::fill(shmPixels, shmPixels + (width * height), kBgColor);

    auto content = app.getRenderContent();
    int fontCellW = fontRenderer.isInitialized() ? fontRenderer.getCellWidth()  : 8;
    int fontCellH = fontRenderer.isInitialized() ? fontRenderer.getCellHeight() : 16;
    int lineSpacing = fontCellH + 2;
    int pad = 8;

    int maxRows = std::max(1, (height - 2 * pad) / lineSpacing);

    // Auto-scroll: render only the latest maxRows lines
    int startLine = std::max(0, static_cast<int>(content.lines.size()) - maxRows);
    int curY = pad;

    for (size_t l = startLine; l < content.lines.size() && curY + fontCellH <= height - pad; ++l) {
        const auto& line = content.lines[l];
        if (!line.empty()) {
            if (fontRenderer.isInitialized()) {
                fontRenderer.renderStringClipped(shmPixels, width, height, pad, curY,
                                                 line, kTextColor, pad, pad, width - pad, height - pad);
            } else {
                drawFallbackString(shmPixels, width, height, pad, curY, line, kTextColor);
            }
        }
        curY += lineSpacing;
    }

    // Draw active cursor at write head
    int lastLineY = curY - lineSpacing;
    if (lastLineY < pad) lastLineY = pad;

    std::string lastLineText = content.lines.empty() ? "" : content.lines.back();
    int caretCol = content.cursorCol;
    std::string caretPrefix = (caretCol >= 0 && caretCol <= static_cast<int>(lastLineText.size()))
                              ? lastLineText.substr(0, caretCol) : lastLineText;

    int caretX = pad + (fontRenderer.isInitialized() ? fontRenderer.getTextWidth(caretPrefix) : caretCol * fontCellW);

    auto now = std::chrono::steady_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    bool showCursor = content.forceCursorSolid || ((ms / 500) % 2 == 0);

    if (showCursor && caretX + fontCellW <= width - pad && lastLineY + fontCellH <= height - pad) {
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
            std::strncpy(addr.sun_path, lcl::core::kCompositorSocket, sizeof(addr.sun_path) - 1);
            if (connect(socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(socketFd);
            socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (socketFd < 0) {
        std::cerr << "[LCL Terminal ERROR] Could not connect to compositor socket: " << lcl::core::kCompositorSocket << "\n";
        return 1;
    }

    std::cout << "[LCL Terminal] Connected to Compositor IPC socket successfully.\n";

    // 2. Register role as CLIENT_APP
    lcl::protocol::LCLHeader regHeader{};
    regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
    regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

    lcl::protocol::LCLMsgRegisterRole regMsg{};
    regMsg.role = lcl::protocol::LCLRole::ClientApp;
    std::strncpy(regMsg.clientName, "lcl-terminal", sizeof(regMsg.clientName) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, regHeader, &regMsg);
    std::cout << "[LCL Terminal] Registered as CLIENT_APP role on lcl-core compositor.\n";

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
    std::cout << "[LCL Terminal] Requested surface creation (ID: 1, " << kSurfW << "x" << kSurfH << ") from compositor.\n";

    // 4. Create Shared Memory (memfd) Framebuffer for Surface 1
    size_t shmSize = kSurfW * kSurfH * 4;
    int shmFd = memfd_create("lcl_terminal_shm", MFD_CLOEXEC);
    uint32_t* shmPixels = nullptr;
    if (shmFd >= 0) {
        ftruncate(shmFd, shmSize);
        std::cout << "[LCL Terminal] Allocated Shared Memory Buffer (memfd: " << shmFd << ", " << shmSize << " bytes).\n";

        shmPixels = reinterpret_cast<uint32_t*>(
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
        "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"
    };
    for (const auto& path : fontPaths) {
        if (fontRenderer.loadFont(path, 15.0f)) {
            std::cout << "[LCL Terminal] Loaded TTF font: " << path << "\n";
            break;
        }
    }

    // 6. Initialize local PTY and terminal shell instance
    lcl::apps::TerminalApp app;
    if (!app.initialize(1)) {
        std::cerr << "[LCL Terminal ERROR] Terminal App PTY initialization failed.\n";
        if (shmPixels) munmap(shmPixels, shmSize);
        if (shmFd >= 0) close(shmFd);
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
    windowApp.setOnRawKeyEvent([&app](const lcl::ui::KeyEvent& ev) {
        app.handleKey(ev.keyCode, ev.type == lcl::ui::KeyEventType::KeyDown);
        return true; // Intercept & consume directly for PTY shell
    });

    int curW = kSurfW;
    int curH = kSurfH;
    bool termNeedsAttach = true;
    std::vector<uint32_t> localPixels(curW * curH, 0xFF14161D);

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
    attachMsg.width  = kSurfW;
    attachMsg.height = kSurfH;
    attachMsg.stride = kSurfW * 4;
    attachMsg.format = 1; // ARGB8888

    lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, shmFd);
    termNeedsAttach = false;
    std::cout << "[LCL Terminal] Sent initial ATTACH_BUFFER (memfd: " << shmFd << ") for Surface 1.\n";
    std::cout << "[LCL Terminal] Standalone process running (PID: " << getpid() << "). PTY shell active.\n";

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
                    auto* inputMsg = reinterpret_cast<const lcl::protocol::LCLMsgInputEvent*>(payload.data());
                    if (inputMsg->type == 1) { // KeyboardKey
                        if (inputMsg->pressed) {
                            windowApp.sendKeyDown(inputMsg->key, 0, inputMsg->modifiers);
                        } else {
                            windowApp.sendKeyUp(inputMsg->key, inputMsg->modifiers);
                        }
                        updated = true;
                    }
                } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                           payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                    auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
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

        // Event Coalescing: Execute resize ONCE for the most recent dimensions in socket queue
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
            localPixels.resize(curW * curH, 0xFF14161D);

            shmFd = memfd_create("lcl_term_shm", MFD_CLOEXEC);
            if (shmFd >= 0) {
                ftruncate(shmFd, shmSize);
                shmPixels = reinterpret_cast<uint32_t*>(
                    mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));

                if (shmPixels == MAP_FAILED) {
                    shmPixels = nullptr;
                } else {
                    std::fill_n(shmPixels, curW * curH, 0xFF14161D);
                }
            }

            app.resize(curW, curH);
            windowApp.getRootWidget()->getYogaNode().setWidth(static_cast<float>(curW));
            windowApp.getRootWidget()->getYogaNode().setHeight(static_cast<float>(curH));

            // Pre-render terminal layout for new dimensions & copy to shmPixels BEFORE committing to Compositor!
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
        auto blinkMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBlink).count();
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

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    app.shutdown();
    if (shmPixels) munmap(shmPixels, shmSize);
    if (shmFd >= 0) close(shmFd);
    close(socketFd);
    return 0;
}
