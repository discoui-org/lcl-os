#include "render/renderer.hpp"
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
    m_softwareBackBuffer.assign(m_width * m_height, 0xFF0F172A);

    if (m_displayManager && m_displayManager->isInitialized()) {
        if (m_displayManager->getBackendType() == core::DisplayBackendType::DRM_KMS) {
            if (createDumbBuffer()) {
                m_usingDRMHardware = true;
                m_displayManager->initHardwareCursor(64, 64);
                std::cout << "[LCL Render] DRM Hardware acceleration active! Driver: "
                          << m_displayManager->getDriverName()
                          << " (Render Node: "
                          << (m_displayManager->getRenderNodePath().empty() ? "Direct KMS" : m_displayManager->getRenderNodePath())
                          << ")\n";
            }
        }
    }

    // Initialize TrueType Vector Font Engine (JetBrains Mono TTF with fallback)
    std::vector<std::string> fontPaths = {
        "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/inter/Inter-Regular.otf",
        "assets/fonts/inter/Inter-Regular.otf"
    };

    for (const auto& path : fontPaths) {
        if (m_fontRenderer.loadFont(path, 15.0f)) {
            break;
        }
    }

    m_initialized = true;
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
    for (int r = 0; r < 16; ++r) {
        uint8_t rowMask = getGlyphRow(c, r);
        for (int col = 0; col < 8; ++col) {
            if ((rowMask >> (7 - col)) & 1) {
                drawPixel(x + col, y + r, fgColor);
            }
        }
    }
}

void Renderer::drawString(int x, int y, const std::string& text, uint32_t fgColor) {
    if (m_fontRenderer.isInitialized()) {
        m_fontRenderer.renderString(m_softwareBackBuffer.data(), m_width, m_height, x, y, text, fgColor);
    } else {
        int curX = x;
        int curY = y;
        for (char c : text) {
            if (c == '\n') {
                curX = x;
                curY += 16;
                continue;
            }
            drawChar(curX, curY, c, fgColor);
            curX += 8;
        }
    }
}

void Renderer::drawCharClipped(int x, int y, char c, uint32_t fgColor, int minX, int minY, int maxX, int maxY) {
    for (int r = 0; r < 16; ++r) {
        int py = y + r;
        if (py < minY || py >= maxY) continue;

        uint8_t rowMask = getGlyphRow(c, r);
        for (int col = 0; col < 8; ++col) {
            int px = x + col;
            if (px < minX || px >= maxX) continue;
            if ((rowMask >> (7 - col)) & 1) {
                drawPixel(px, py, fgColor);
            }
        }
    }
}

void Renderer::drawStringClipped(int x, int y, const std::string& text, uint32_t fgColor, int minX, int minY, int maxX, int maxY) {
    if (m_fontRenderer.isInitialized()) {
        m_fontRenderer.renderStringClipped(m_softwareBackBuffer.data(), m_width, m_height, x, y, text, fgColor, minX, minY, maxX, maxY);
    } else {
        int curX = x;
        int curY = y;
        for (char c : text) {
            if (c == '\n') {
                curX = x;
                curY += 16;
                continue;
            }
            drawCharClipped(curX, curY, c, fgColor, minX, minY, maxX, maxY);
            curX += 8;
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

    for (int j = startY; j < endY; ++j) {
        for (int i = startX; i < endX; ++i) {
            m_softwareBackBuffer[j * m_width + i] = argbColor;
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

    for (int r = 0; r < 16; ++r) {
        for (int c = 0; c < 12; ++c) {
            char ch = cursorShape[r][c];
            if (ch == 'X') {
                drawPixel(mouseX + c, mouseY + r, 0xFF000000);
            } else if (ch == '.') {
                drawPixel(mouseX + c, mouseY + r, 0xFFFFFFFF);
            }
        }
    }
}

void Renderer::drawWindowFrame(int x, int y, int width, int height, const std::string& title, uint32_t headerColor) {
    // Window header bar
    drawFilledRect(x, y, width, 32, headerColor);
    // Window body
    drawFilledRect(x, y + 32, width, height - 32, 0xFF1E293B);
    // Window border
    drawRect(x, y, width, height, 0xFF38BDF8);
    // Window title text
    drawString(x + 70, y + 8, title, 0xFFFFFFFF);
    // Window control buttons
    drawFilledRect(x + 10, y + 10, 12, 12, 0xFFEF4444); // Red
    drawFilledRect(x + 28, y + 10, 12, 12, 0xFFF59E0B); // Yellow
    drawFilledRect(x + 46, y + 10, 12, 12, 0xFF10B981); // Green
}

void Renderer::renderLCLDesktopShell(const std::string& statusMessage) {
    (void)statusMessage;
    clear(0xFF090D16);
    drawFilledRect(0, 0, m_width, 40, 0xFF1E1E2E);
    drawRect(0, 39, m_width, 1, 0xFF45475A);
    drawFilledRect(10, 6, 80, 28, 0xFF89B4FA);
    drawString(20, 12, "LCL OS", 0xFF000000);
    drawWindowFrame(80, 80, 540, 360, "LCL Terminal / Core Engine", 0xFF89B4FA);
    drawWindowFrame(360, 200, 460, 300, "LCL System Monitor", 0xFF45475A);
    drawCursor(512, 384);
}

void Renderer::renderDesktop(const WindowManager& windowManager, const std::vector<WindowRenderContent>& windowContents) {
    // 1. Wallpaper background
    clear(0xFF090D16);

    // 2. Top Taskbar / Shell Panel
    drawFilledRect(0, 0, m_width, 40, 0xFF1E1E2E);
    drawRect(0, 39, m_width, 1, 0xFF45475A);

    // LCL Shell Logo Indicator & System Title
    drawFilledRect(10, 6, 90, 28, 0xFF89B4FA);
    drawString(20, 12, "LCL Core", 0xFF1E1E2E);
    std::string hwInfo = "LCL OS v0.1.0 (" + (m_displayManager && m_displayManager->isHardwareAccelerated() ? m_displayManager->getDriverName() : "DRM FB") + ")";
    drawString(m_width - static_cast<int>(hwInfo.length() * 8 + 20), 12, hwInfo, 0xFFA6ADC8);

    // 3. Render Windows in z-order
    for (const auto& win : windowManager.getWindows()) {
        drawWindowFrame(win.x, win.y, win.width, win.height, win.title, win.headerColor);

        // Find matching WindowRenderContent for this window ID
        const WindowRenderContent* contentPtr = nullptr;
        for (const auto& content : windowContents) {
            if (content.windowId == win.id) {
                contentPtr = &content;
                break;
            }
        }

        if (contentPtr && !contentPtr->lines.empty()) {
            const auto& lines = contentPtr->lines;
            int minX = win.x + 12;
            int minY = win.y + 40;
            int maxX = win.x + win.width - 12;
            int maxY = win.y + win.height - 12;

            int fontCellWidth = m_fontRenderer.isInitialized() ? m_fontRenderer.getCellWidth() : 8;
            int fontCellHeight = m_fontRenderer.isInitialized() ? m_fontRenderer.getCellHeight() : 16;
            int lineSpacing = fontCellHeight + 2;

            int maxCols = std::max(1, (maxX - minX) / fontCellWidth);
            int maxRows = std::max(1, (maxY - minY) / lineSpacing);

            // Auto-wrap lines that exceed maxCols
            std::vector<std::string> wrappedLines;
            for (const auto& line : lines) {
                if (line.empty()) {
                    wrappedLines.push_back("");
                    continue;
                }
                for (size_t i = 0; i < line.size(); i += maxCols) {
                    wrappedLines.push_back(line.substr(i, maxCols));
                }
            }

            // Auto-scroll to show latest maxRows lines
            int startLine = std::max(0, static_cast<int>(wrappedLines.size()) - maxRows);
            int curY = minY;
            int lastLineY = minY;
            std::string lastLineText;

            for (size_t l = startLine; l < wrappedLines.size() && curY + fontCellHeight <= maxY; ++l) {
                drawStringClipped(minX, curY, wrappedLines[l], 0xFFA6E3A1, minX, minY, maxX, maxY);
                lastLineY = curY;
                lastLineText = wrappedLines[l];
                curY += lineSpacing;
            }

            // --- 500ms Blinking Text Cursor (Font-Agnostic Inverted Block Cursor) ---
            int caretByteOffset = contentPtr->cursorCol;
            std::string caretPrefix;
            std::string charUnderCursor;

            if (caretByteOffset >= 0 && caretByteOffset <= static_cast<int>(lastLineText.size())) {
                int safeOffset = caretByteOffset;
                while (safeOffset > 0 && safeOffset < static_cast<int>(lastLineText.size()) &&
                       (static_cast<unsigned char>(lastLineText[safeOffset]) & 0xC0) == 0x80) {
                    --safeOffset;
                }
                caretPrefix = lastLineText.substr(0, safeOffset);

                if (safeOffset < static_cast<int>(lastLineText.size())) {
                    unsigned char firstByte = lastLineText[safeOffset];
                    size_t charLen = 1;
                    if      ((firstByte & 0xE0) == 0xC0) charLen = 2;
                    else if ((firstByte & 0xF0) == 0xE0) charLen = 3;
                    else if ((firstByte & 0xF8) == 0xF0) charLen = 4;

                    if (safeOffset + charLen <= lastLineText.size()) {
                        charUnderCursor = lastLineText.substr(safeOffset, charLen);
                    }
                }
            } else {
                caretPrefix = lastLineText;
            }

            int caretPixelX = m_fontRenderer.isInitialized()
                ? m_fontRenderer.getTextWidth(caretPrefix)
                : static_cast<int>(caretPrefix.size()) * fontCellWidth;
            int caretX = minX + caretPixelX;
            int caretY = lastLineY;

            auto now = std::chrono::steady_clock::now();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            bool showCursor = contentPtr->forceCursorSolid || ((millis / 500) % 2 == 0);

            if (showCursor && !wrappedLines.empty()) {
                int cursorBoxWidth = (!charUnderCursor.empty() && m_fontRenderer.isInitialized())
                    ? std::max(4, m_fontRenderer.getTextWidth(charUnderCursor))
                    : fontCellWidth;

                if (caretX + cursorBoxWidth <= maxX && caretY + fontCellHeight <= maxY) {
                    // 1. Draw solid light slate cursor block
                    drawFilledRect(caretX, caretY, cursorBoxWidth, fontCellHeight, 0xFFE2E8F0);

                    // 2. Draw inverted character under cursor in dark navy ink (0xFF0F172A)
                    if (!charUnderCursor.empty()) {
                        drawStringClipped(caretX, caretY, charUnderCursor, 0xFF0F172A, minX, minY, maxX, maxY);
                    }
                }
            }
        }
    }

    // 4. Render Mouse Cursor on top
    if (m_displayManager && m_displayManager->isHardwareCursorActive()) {
        m_displayManager->moveHardwareCursor(windowManager.getMouseX(), windowManager.getMouseY());
    } else {
        drawCursor(windowManager.getMouseX(), windowManager.getMouseY());
    }
}

void Renderer::swapBuffers() {
    m_renderedFrames++;

    if (m_displayManager && m_displayManager->isInitialized()) {
        if (m_usingDRMHardware && m_dumbBuffer.pixelData && m_dumbBuffer.fbId > 0) {
            std::memcpy(m_dumbBuffer.pixelData, m_softwareBackBuffer.data(), std::min(m_dumbBuffer.size, m_softwareBackBuffer.size() * sizeof(uint32_t)));

            int drmFd = m_displayManager->getDRMFd();
            if (drmFd >= 0) {
                drmModeDirtyFB(drmFd, m_dumbBuffer.fbId, nullptr, 0);
            }
        } else if (m_displayManager->getBackendType() == core::DisplayBackendType::LinuxFB) {
            uint32_t* fbPixels = m_displayManager->getFBPixelData();
            if (fbPixels) {
                size_t copyBytes = std::min(static_cast<size_t>(m_width * m_height * sizeof(uint32_t)),
                                            static_cast<size_t>(m_displayManager->getFBDevice().size));
                std::memcpy(fbPixels, m_softwareBackBuffer.data(), copyBytes);
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
