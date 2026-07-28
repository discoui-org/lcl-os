#include "render/renderer.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"
#include <iostream>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <chrono>

namespace {

// Helper to get 8x16 VGA font glyph row mask
uint8_t getGlyphRow(char c, int row) {
    if (row < 0 || row >= 16) return 0;

    // Numbers 0..9
    if (c >= '0' && c <= '9') {
        static const uint8_t digits[10][16] = {
            {0x00,0x00,0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0
            {0x00,0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 1
            {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 2
            {0x00,0x00,0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 3
            {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 4
            {0x00,0x00,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 5
            {0x00,0x00,0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 6
            {0x00,0x00,0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 7
            {0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 8
            {0x00,0x00,0x3C,0x66,0x66,0x3E,0x06,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // 9
        };
        return digits[c - '0'][row];
    }

    // Uppercase A..Z
    if (c >= 'A' && c <= 'Z') {
        static const uint8_t upper[26][16] = {
            {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // A
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,0x00,0x00}, // B
            {0x00,0x00,0x3C,0x66,0xC0,0xC0,0xC0,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00}, // C
            {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00,0x00}, // D
            {0x00,0x00,0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // E
            {0x00,0x00,0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // F
            {0x00,0x00,0x3C,0x66,0xC0,0xC0,0xCE,0xC6,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00}, // G
            {0x00,0x00,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00}, // H
            {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00}, // I
            {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00,0x00}, // J
            {0x00,0x00,0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // K
            {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x62,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // L
            {0x00,0x00,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // M
            {0x00,0x00,0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // N
            {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // O
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // P
            {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xDA,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00}, // Q
            {0x00,0x00,0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // R
            {0x00,0x00,0x3E,0x66,0x60,0x3C,0x06,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // S
            {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // T
            {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // U
            {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // V
            {0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // W
            {0x00,0x00,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // X
            {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Y
            {0x00,0x00,0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // Z
        };
        return upper[c - 'A'][row];
    }

    // Lowercase a..z
    if (c >= 'a' && c <= 'z') {
        static const uint8_t lower[26][16] = {
            {0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // a
            {0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // b
            {0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // c
            {0x00,0x00,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // d
            {0x00,0x00,0x00,0x3C,0x66,0x7E,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // e
            {0x00,0x00,0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // f
            {0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // g
            {0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // h
            {0x00,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // i
            {0x00,0x00,0x06,0x00,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // j
            {0x00,0x00,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // k
            {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // l
            {0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // m
            {0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // n
            {0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // o
            {0x00,0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // p
            {0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // q
            {0x00,0x00,0x00,0x5C,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // r
            {0x00,0x00,0x00,0x3E,0x60,0x3C,0x06,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // s
            {0x00,0x00,0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // t
            {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // u
            {0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // v
            {0x00,0x00,0x00,0xC6,0xC6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // w
            {0x00,0x00,0x00,0x66,0x3C,0x18,0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // x
            {0x00,0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // y
            {0x00,0x00,0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // z
        };
        return lower[c - 'a'][row];
    }

    // Special symbols
    switch (c) {
        case '/': return uint8_t("\x00\x00\x02\x06\x0C\x18\x30\x60\x40\x00\x00\x00\x00\x00\x00\x00"[row]);
        case ':': return uint8_t("\x00\x00\x00\x18\x18\x00\x18\x18\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '$': return uint8_t("\x00\x18\x3E\x68\x3C\x0B\x7C\x18\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '#': return uint8_t("\x00\x24\x24\x7E\x24\x7E\x24\x24\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '-': return uint8_t("\x00\x00\x00\x00\x7E\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '_': return uint8_t("\x00\x00\x00\x00\x00\x00\x00\x00\xFE\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '.': return uint8_t("\x00\x00\x00\x00\x00\x18\x18\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '<': return uint8_t("\x00\x00\x0C\x18\x30\x60\x30\x18\x0C\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '>': return uint8_t("\x00\x00\x30\x18\x0C\x06\x0C\x18\x30\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '=': return uint8_t("\x00\x00\x00\x7E\x00\x7E\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '!': return uint8_t("\x00\x00\x18\x18\x18\x18\x00\x18\x00\x00\x00\x00\x00\x00\x00\x00"[row]);
        case '?': return uint8_t("\x00\x00\x3C\x66\x0C\x18\x18\x00\x18\x00\x00\x00\x00\x00\x00\x00"[row]);
        default: return 0x00;
    }
}

} // namespace

namespace lcl::render {

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

Renderer::Renderer(Renderer&& other) noexcept
    : m_displayManager(other.m_displayManager),
      m_width(other.m_width),
      m_height(other.m_height),
      m_initialized(other.m_initialized),
      m_usingDRMHardware(other.m_usingDRMHardware),
      m_dumbBuffer(other.m_dumbBuffer),
      m_softwareBackBuffer(std::move(other.m_softwareBackBuffer)),
      m_renderedFrames(other.m_renderedFrames) {
    other.m_displayManager = nullptr;
    other.m_dumbBuffer = Framebuffer{};
    other.m_initialized = false;
    other.m_usingDRMHardware = false;
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_displayManager = other.m_displayManager;
        m_width = other.m_width;
        m_height = other.m_height;
        m_initialized = other.m_initialized;
        m_usingDRMHardware = other.m_usingDRMHardware;
        m_dumbBuffer = other.m_dumbBuffer;
        m_softwareBackBuffer = std::move(other.m_softwareBackBuffer);
        m_renderedFrames = other.m_renderedFrames;

        other.m_displayManager = nullptr;
        other.m_dumbBuffer = Framebuffer{};
        other.m_initialized = false;
        other.m_usingDRMHardware = false;
    }
    return *this;
}

bool Renderer::initialize(core::DisplayManager* displayManager) {
    if (m_initialized) return true;

    m_displayManager = displayManager;
    if (m_displayManager && m_displayManager->isInitialized()) {
        const auto& mode = m_displayManager->getActiveDisplayMode();
        if (mode.width > 0 && mode.height > 0) {
            m_width = mode.width;
            m_height = mode.height;
        }
    }

    std::cout << "[LCL Render] Initializing Renderer Engine (" << m_width << "x" << m_height << ")...\n";
    m_softwareBackBuffer.assign(m_width * m_height, 0xFF000000);

    if (m_displayManager && m_displayManager->isInitialized()) {
        if (m_displayManager->getBackendType() == core::DisplayBackendType::DRM_KMS) {
            if (createDumbBuffer()) {
                m_usingDRMHardware = true;
                // Hardware cursor buffer sized for scaled arrow (base 12x16 × DPR)
                const int cs = std::max(1, core::DisplayScale::px(1));
                const uint32_t cursorDim = static_cast<uint32_t>(
                    std::max(64, std::max(12 * cs, 16 * cs) + 8));
                m_displayManager->initHardwareCursor(cursorDim, cursorDim);
                std::cout << "[LCL Render] DRM Hardware acceleration active! Driver: "
                          << m_displayManager->getDriverName()
                          << " (Render Node: "
                          << (m_displayManager->getRenderNodePath().empty() ? "Direct KMS" : m_displayManager->getRenderNodePath())
                          << ")\n";
            }
        }
    }

    // Initialize Skia Hardware / Software Rendering Backend
    core::EGLBackend* eglBackend = m_displayManager ? m_displayManager->getEGLBackend() : nullptr;
    m_skiaRenderer.initialize(m_width, m_height, eglBackend, m_softwareBackBuffer.data());

    // Initialize TrueType Vector Font Engine (JetBrains Mono TTF with fallback)
    std::vector<std::string> fontPaths = {
        "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/inter/Inter-Regular.otf",
        "assets/fonts/inter/Inter-Regular.otf"
    };

    const float fontPx = core::DisplayScale::fontSize();
    for (const auto& path : fontPaths) {
        if (m_fontRenderer.loadFont(path, fontPx)) {
            break;
        }
    }

    m_initialized = true;
    std::cout << "[LCL Render] UI scale: " << core::DisplayScale::factor()
              << "  font: " << fontPx << "px\n";
    return true;
}

bool Renderer::createDumbBuffer() {
    if (!m_displayManager || m_displayManager->getDRMFd() < 0) return false;

    int drmFd = m_displayManager->getDRMFd();
    const auto& drmDevice = m_displayManager->getDRMDevice();
    if (!drmDevice.crtc || !drmDevice.connector) return false;

    uint32_t crtcId = drmDevice.crtc->crtc_id;
    uint32_t connectorId = drmDevice.connector->connector_id;
    auto modeInfo = drmDevice.currentMode;

    struct drm_mode_create_dumb creq{};
    creq.width = m_width;
    creq.height = m_height;
    creq.bpp = 32;

    if (ioctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return false;

    m_dumbBuffer.width = m_width;
    m_dumbBuffer.height = m_height;
    m_dumbBuffer.pitch = creq.pitch;
    m_dumbBuffer.handle = creq.handle;
    m_dumbBuffer.size = creq.size;

    if (drmModeAddFB(drmFd, m_width, m_height, 24, 32, creq.pitch, creq.handle, &m_dumbBuffer.fbId) != 0) return false;

    struct drm_mode_map_dumb mreq{};
    mreq.handle = creq.handle;
    if (ioctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) return false;

    void* mapPtr = mmap(nullptr, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd, mreq.offset);
    if (mapPtr == MAP_FAILED) return false;

    m_dumbBuffer.pixelData = static_cast<uint32_t*>(mapPtr);

    if (drmModeSetCrtc(drmFd, crtcId, m_dumbBuffer.fbId, 0, 0, &connectorId, 1, &modeInfo) != 0) {
        std::cerr << "[LCL Render WARNING] drmModeSetCrtc failed.\n";
    } else {
        std::cout << "[LCL Render] DRM Modeset active on CRTC ID: " << crtcId << " (FB ID: " << m_dumbBuffer.fbId << ")\n";
    }

    return true;
}

void Renderer::clear(uint32_t argbColor) {
    std::fill(m_softwareBackBuffer.begin(), m_softwareBackBuffer.end(), argbColor);
}

void Renderer::drawPixel(int x, int y, uint32_t argbColor) {
    if (x < 0 || x >= static_cast<int>(m_width) || y < 0 || y >= static_cast<int>(m_height)) return;
    m_softwareBackBuffer[y * m_width + x] = argbColor;
}

void Renderer::drawChar(int x, int y, char c, uint32_t fgColor) {
    const int s = core::DisplayScale::px(1);
    for (int r = 0; r < 16; ++r) {
        uint8_t rowMask = getGlyphRow(c, r);
        for (int col = 0; col < 8; ++col) {
            if ((rowMask >> (7 - col)) & 1) {
                if (s <= 1) {
                    drawPixel(x + col, y + r, fgColor);
                } else {
                    drawFilledRect(x + col * s, y + r * s, s, s, fgColor);
                }
            }
        }
    }
}

void Renderer::drawString(int x, int y, const std::string& text, uint32_t fgColor) {
    if (m_fontRenderer.isInitialized()) {
        m_fontRenderer.renderString(m_softwareBackBuffer.data(), m_width, m_height, x, y, text, fgColor);
    } else {
        const int cellW = core::DisplayScale::px(8);
        const int cellH = core::DisplayScale::px(16);
        int curX = x;
        int curY = y;
        for (char c : text) {
            if (c == '\n') {
                curX = x;
                curY += cellH;
                continue;
            }
            drawChar(curX, curY, c, fgColor);
            curX += cellW;
        }
    }
}

void Renderer::drawCharClipped(int x, int y, char c, uint32_t fgColor, int minX, int minY, int maxX, int maxY) {
    const int s = std::max(1, core::DisplayScale::px(1));
    for (int r = 0; r < 16; ++r) {
        for (int col = 0; col < 8; ++col) {
            if (!((getGlyphRow(c, r) >> (7 - col)) & 1)) {
                continue;
            }
            if (s <= 1) {
                int px = x + col;
                int py = y + r;
                if (px < minX || px >= maxX || py < minY || py >= maxY) {
                    continue;
                }
                drawPixel(px, py, fgColor);
            } else {
                int px = x + col * s;
                int py = y + r * s;
                // Clip filled block roughly against bounds
                int w = s;
                int h = s;
                if (px + w <= minX || py + h <= minY || px >= maxX || py >= maxY) {
                    continue;
                }
                drawFilledRect(px, py, w, h, fgColor);
            }
        }
    }
}

void Renderer::drawStringClipped(int x, int y, const std::string& text, uint32_t fgColor, int minX, int minY, int maxX, int maxY) {
    if (m_fontRenderer.isInitialized()) {
        m_fontRenderer.renderStringClipped(m_softwareBackBuffer.data(), m_width, m_height, x, y, text, fgColor, minX, minY, maxX, maxY);
    } else {
        const int cellW = core::DisplayScale::px(8);
        const int cellH = core::DisplayScale::px(16);
        int curX = x;
        int curY = y;
        for (char c : text) {
            if (c == '\n') {
                curX = x;
                curY += cellH;
                continue;
            }
            drawCharClipped(curX, curY, c, fgColor, minX, minY, maxX, maxY);
            curX += cellW;
        }
    }
}

void Renderer::drawRect(int x, int y, int width, int height, uint32_t argbColor) {
    for (int i = x; i < x + width; ++i) {
        drawPixel(i, y, argbColor);
        drawPixel(i, y + height - 1, argbColor);
    }
    for (int j = y; j < y + height; ++j) {
        drawPixel(x, j, argbColor);
        drawPixel(x + width - 1, j, argbColor);
    }
}

void Renderer::drawFilledRect(int x, int y, int width, int height, uint32_t argbColor) {
    int startX = std::max(0, x);
    int endX = std::min(static_cast<int>(m_width), x + width);
    int startY = std::max(0, y);
    int endY = std::min(static_cast<int>(m_height), y + height);

    uint8_t a = (argbColor >> 24) & 0xFF;
    if (a == 255) {
        for (int j = startY; j < endY; ++j) {
            std::fill_n(&m_softwareBackBuffer[j * m_width + startX], endX - startX, argbColor);
        }
    } else if (a > 0) {
        float alpha = a / 255.0f;
        float invAlpha = 1.0f - alpha;
        uint8_t srcR = (argbColor >> 16) & 0xFF;
        uint8_t srcG = (argbColor >> 8) & 0xFF;
        uint8_t srcB = argbColor & 0xFF;

        for (int j = startY; j < endY; ++j) {
            for (int i = startX; i < endX; ++i) {
                uint32_t bg = m_softwareBackBuffer[j * m_width + i];
                uint8_t bgR = (bg >> 16) & 0xFF;
                uint8_t bgG = (bg >> 8) & 0xFF;
                uint8_t bgB = bg & 0xFF;

                uint8_t r = static_cast<uint8_t>(srcR * alpha + bgR * invAlpha);
                uint8_t g = static_cast<uint8_t>(srcG * alpha + bgG * invAlpha);
                uint8_t b = static_cast<uint8_t>(srcB * alpha + bgB * invAlpha);

                m_softwareBackBuffer[j * m_width + i] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void Renderer::drawCursor(int mouseX, int mouseY) {
    static const char* cursorShape[] = {
        "X           ",
        "XX          ",
        "X.X         ",
        "X..X        ",
        "X...X       ",
        "X....X      ",
        "X.....X     ",
        "X......X    ",
        "X.......X   ",
        "X........X  ",
        "X.....XXXXX ",
        "X..X..X     ",
        "X.X X..X    ",
        "XX   X..X   ",
        "X    X..X   ",
        "      XX    "
    };

    const int s = std::max(1, core::DisplayScale::px(1));
    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 12; ++c) {
            char ch = cursorShape[r][c];
            if (ch == 'X') {
                if (s <= 1) {
                    drawPixel(mouseX + c, mouseY + r, 0xFF000000);
                } else {
                    drawFilledRect(mouseX + c * s, mouseY + r * s, s, s, 0xFF000000);
                }
            } else if (ch == '.') {
                if (s <= 1) {
                    drawPixel(mouseX + c, mouseY + r, 0xFFFFFFFF);
                } else {
                    drawFilledRect(mouseX + c * s, mouseY + r * s, s, s, 0xFFFFFFFF);
                }
            }
        }
    }
}

void Renderer::drawWindowFrame(int x, int y, int width, int height, const std::string& title, uint32_t headerColor) {
    using core::DisplayScale;
    const int titleH = DisplayScale::titleBarHeight();
    const int btn = DisplayScale::trafficBtn();
    const int gap = DisplayScale::trafficGap();
    const int pad = DisplayScale::px(10);
    const int titleTx = DisplayScale::px(70);
    const int titleTy = DisplayScale::px(8);

    drawFilledRect(x, y, width, titleH, headerColor);
    drawFilledRect(x, y + titleH, width, height - titleH, lcl::theme::UI::WindowBodyBg);
    drawRect(x, y, width, height, lcl::theme::UI::WindowBorder);
    drawString(x + titleTx, y + titleTy, title, lcl::theme::UI::WindowTitleText);
    drawFilledRect(x + pad, y + pad, btn, btn, lcl::theme::UI::BtnClose);
    drawFilledRect(x + pad + gap, y + pad, btn, btn, lcl::theme::UI::BtnMinimize);
    drawFilledRect(x + pad + gap * 2, y + pad, btn, btn, lcl::theme::UI::BtnMaximize);
}

void Renderer::renderLCLDesktopShell(const std::string& statusMessage) {
    (void)statusMessage;
    clear(0xFF000000);
    drawCursor(static_cast<int>(m_width / 2), static_cast<int>(m_height / 2));
}

void Renderer::renderDesktop(const WindowManager& windowManager,
                              const std::vector<WindowRenderContent>& windowContents) {
    // beginFrame() is called by the compositor before renderDesktop() — do NOT call it here.

    // 1. Render registered client surfaces/windows (with text-based fallback content)
    for (const auto& win : windowManager.getWindows()) {
        const WindowRenderContent* content = nullptr;
        for (const auto& c : windowContents) {
            if (c.windowId == win.id) { content = &c; break; }
        }
        renderWindowContent(win, content);
    }

    // 2. Mouse cursor on top of everything
    if (m_displayManager && m_displayManager->isHardwareCursorActive()) {
        m_displayManager->moveHardwareCursor(windowManager.getMouseX(), windowManager.getMouseY());
    } else {
        drawCursor(windowManager.getMouseX(), windowManager.getMouseY());
    }
}

// -----------------------------------------------------------------------
// Private render passes
// -----------------------------------------------------------------------

void Renderer::renderBackground() {
    clear(0xFF000000);
}

void Renderer::renderTaskbar() {
    // No-op: Compositor core does not render built-in taskbars/panels (handled via lcl-protocol layer-shell clients)
}

void Renderer::renderWindowContent(const Window& win, const WindowRenderContent* content) {
    using core::DisplayScale;
    const int pad        = DisplayScale::windowPad();
    const int titleH     = DisplayScale::titleBarHeight();
    const int contentTop = titleH + DisplayScale::px(8);

    // Window chrome (title bar + body + border + traffic-light buttons)
    drawWindowFrame(win.x, win.y, win.width, win.height, win.title, win.headerColor);

    if (!content || content->lines.empty()) return;

    // Content clipping rectangle
    int minX = win.x + pad;
    int minY = win.y + contentTop;
    int maxX = win.x + win.width  - pad;
    int maxY = win.y + win.height - pad;

    // Font metrics (TTF or VGA fallback)
    int fontCellW  = m_fontRenderer.isInitialized() ? m_fontRenderer.getCellWidth()  : DisplayScale::px(8);
    int fontCellH  = m_fontRenderer.isInitialized() ? m_fontRenderer.getCellHeight() : DisplayScale::px(16);
    int lineSpacing = fontCellH + DisplayScale::px(2);

    int maxCols = std::max(1, (maxX - minX) / fontCellW);
    int maxRows = std::max(1, (maxY - minY) / lineSpacing);

    // Hard-wrap lines that exceed maxCols columns
    std::vector<std::string> wrapped;
    for (const auto& line : content->lines) {
        if (line.empty()) { wrapped.push_back(""); continue; }
        for (size_t i = 0; i < line.size(); i += maxCols) {
            wrapped.push_back(line.substr(i, maxCols));
        }
    }

    // Auto-scroll: show only the latest maxRows lines
    int startLine   = std::max(0, static_cast<int>(wrapped.size()) - maxRows);
    int curY        = minY;
    int lastLineY   = minY;
    std::string lastLineText;

    for (size_t l = startLine; l < wrapped.size() && curY + fontCellH <= maxY; ++l) {
        drawStringClipped(minX, curY, wrapped[l],
                          lcl::theme::UI::TerminalText, minX, minY, maxX, maxY);
        lastLineY    = curY;
        lastLineText = wrapped[l];
        curY += lineSpacing;
    }

    // ----------------------------------------------------------------
    // 500ms Blinking Inverted Block Cursor
    // ----------------------------------------------------------------
    int caretByte = content->cursorCol;
    std::string caretPrefix;
    std::string charUnderCursor;

    if (caretByte >= 0 && caretByte <= static_cast<int>(lastLineText.size())) {
        // Walk back to UTF-8 codepoint boundary
        int safe = caretByte;
        while (safe > 0 && safe < static_cast<int>(lastLineText.size()) &&
               (static_cast<unsigned char>(lastLineText[safe]) & 0xC0) == 0x80) {
            --safe;
        }
        caretPrefix = lastLineText.substr(0, safe);

        if (safe < static_cast<int>(lastLineText.size())) {
            unsigned char fb = static_cast<unsigned char>(lastLineText[safe]);
            size_t charLen = 1;
            if      ((fb & 0xE0) == 0xC0) charLen = 2;
            else if ((fb & 0xF0) == 0xE0) charLen = 3;
            else if ((fb & 0xF8) == 0xF0) charLen = 4;
            if (safe + charLen <= lastLineText.size()) {
                charUnderCursor = lastLineText.substr(safe, charLen);
            }
        }
    } else {
        caretPrefix = lastLineText;
    }

    int caretPixelX = m_fontRenderer.isInitialized()
        ? m_fontRenderer.getTextWidth(caretPrefix)
        : static_cast<int>(caretPrefix.size()) * fontCellW;
    int caretX = minX + caretPixelX;
    int caretY = lastLineY;

    auto now    = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count();
    bool showCursor = content->forceCursorSolid || ((millis / 500) % 2 == 0);

    if (showCursor && !wrapped.empty()) {
        int cursorBoxW = (!charUnderCursor.empty() && m_fontRenderer.isInitialized())
            ? std::max(4, m_fontRenderer.getTextWidth(charUnderCursor))
            : fontCellW;

        if (caretX + cursorBoxW <= maxX && caretY + fontCellH <= maxY) {
            // Solid block background
            drawFilledRect(caretX, caretY, cursorBoxW, fontCellH, lcl::theme::UI::CursorBlock);
            // Inverted character under cursor
            if (!charUnderCursor.empty()) {
                drawStringClipped(caretX, caretY, charUnderCursor,
                                  lcl::theme::UI::CursorText, minX, minY, maxX, maxY);
            }
        }
    }
}

void Renderer::swapBuffers() {
    m_renderedFrames++;

    if (m_skiaRenderer.getBackendType() == SkiaBackendType::OpenGL_EGL) {
        m_skiaRenderer.endFrame();
    }

    if (m_displayManager && m_displayManager->isInitialized()) {
        const uint32_t* srcPixels = m_skiaRenderer.getRasterBuffer() ? m_skiaRenderer.getRasterBuffer() : m_softwareBackBuffer.data();
        if (m_usingDRMHardware && m_dumbBuffer.pixelData && m_dumbBuffer.fbId > 0) {
            std::memcpy(m_dumbBuffer.pixelData, srcPixels, std::min(m_dumbBuffer.size, m_width * m_height * sizeof(uint32_t)));

            int drmFd = m_displayManager->getDRMFd();
            if (drmFd >= 0) {
                drmModeDirtyFB(drmFd, m_dumbBuffer.fbId, nullptr, 0);
            }
        } else if (m_displayManager->getBackendType() == core::DisplayBackendType::LinuxFB) {
            uint32_t* fbPixels = m_displayManager->getFBPixelData();
            if (fbPixels) {
                size_t copyBytes = std::min(static_cast<size_t>(m_width * m_height * sizeof(uint32_t)),
                                            static_cast<size_t>(m_displayManager->getFBDevice().size));
                std::memcpy(fbPixels, srcPixels, copyBytes);
            }
        }
    }
}

void Renderer::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL Render] Shutting down Renderer Engine (Total frames rendered: " << m_renderedFrames << ")...\n";
    destroyDumbBuffer();
    m_initialized = false;
}

void Renderer::destroyDumbBuffer() {
    if (m_dumbBuffer.pixelData && m_dumbBuffer.size > 0) {
        munmap(m_dumbBuffer.pixelData, m_dumbBuffer.size);
        m_dumbBuffer.pixelData = nullptr;
    }
    if (m_dumbBuffer.fbId > 0 && m_displayManager && m_displayManager->getDRMFd() >= 0) {
        drmModeRmFB(m_displayManager->getDRMFd(), m_dumbBuffer.fbId);
        m_dumbBuffer.fbId = 0;
    }
    if (m_dumbBuffer.handle > 0 && m_displayManager && m_displayManager->getDRMFd() >= 0) {
        struct drm_mode_destroy_dumb dreq{};
        dreq.handle = m_dumbBuffer.handle;
        ioctl(m_displayManager->getDRMFd(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        m_dumbBuffer.handle = 0;
    }
}

} // namespace lcl::render
