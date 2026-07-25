#include "render/renderer.hpp"
#include <iostream>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <algorithm>

namespace lcl::render {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    shutdown();
}

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
    if (m_initialized) {
        std::cout << "[LCL Render] Renderer already initialized.\n";
        return true;
    }

    m_displayManager = displayManager;
    if (m_displayManager && m_displayManager->isInitialized()) {
        const auto& mode = m_displayManager->getActiveDisplayMode();
        if (mode.width > 0 && mode.height > 0) {
            m_width = mode.width;
            m_height = mode.height;
        }
    }

    std::cout << "[LCL Render] Initializing Renderer Engine (" << m_width << "x" << m_height << ")...\n";

    // Allocate software back buffer
    m_softwareBackBuffer.assign(m_width * m_height, 0xFF0F172A); // Dark slate background

    // Try DRM Dumb Buffer creation if DRM KMS hardware display is active
    if (m_displayManager && m_displayManager->isInitialized()) {
        if (m_displayManager->getBackendType() == core::DisplayBackendType::DRM_KMS) {
            if (createDumbBuffer()) {
                m_usingDRMHardware = true;
                std::cout << "[LCL Render] DRM Dumb Framebuffer Hardware acceleration enabled!\n";
            }
        } else if (m_displayManager->getBackendType() == core::DisplayBackendType::LinuxFB) {
            std::cout << "[LCL Render] Linux Framebuffer (/dev/fb0) direct output enabled!\n";
        }
    }

    if (!m_usingDRMHardware && (!m_displayManager || m_displayManager->getBackendType() == core::DisplayBackendType::None)) {
        std::cout << "[LCL Render] Operating in Software Canvas fallback mode.\n";
    }

    m_initialized = true;
    return true;
}

bool Renderer::createDumbBuffer() {
    return false; // Hardware DRM dumb buffer hook stub
}

void Renderer::clear(uint32_t argbColor) {
    std::fill(m_softwareBackBuffer.begin(), m_softwareBackBuffer.end(), argbColor);
}

void Renderer::drawPixel(int x, int y, uint32_t argbColor) {
    if (x < 0 || x >= static_cast<int>(m_width) || y < 0 || y >= static_cast<int>(m_height)) return;
    m_softwareBackBuffer[y * m_width + x] = argbColor;
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

void Renderer::drawWindowFrame(int x, int y, int width, int height, const std::string& title, uint32_t headerColor) {
    (void)title;
    // Window header bar
    drawFilledRect(x, y, width, 32, headerColor);
    // Window body
    drawFilledRect(x, y + 32, width, height - 32, 0xFF1E293B);
    // Window border
    drawRect(x, y, width, height, 0xFF38BDF8);
    // Window control buttons (Close, Min, Max)
    drawFilledRect(x + 10, y + 10, 12, 12, 0xFFEF4444); // Red
    drawFilledRect(x + 28, y + 10, 12, 12, 0xFFF59E0B); // Yellow
    drawFilledRect(x + 46, y + 10, 12, 12, 0xFF10B981); // Green
}

void Renderer::renderLCLDesktopShell(const std::string& statusMessage) {
    (void)statusMessage;
    // Desktop Wallpaper Surface
    clear(0xFF090D16); // Deep space dark wallpaper

    // Top Status Taskbar / Panel
    drawFilledRect(0, 0, m_width, 40, 0xFF1E1E2E);
    drawRect(0, 39, m_width, 1, 0xFF45475A);

    // LCL Shell Logo / Launcher Indicator
    drawFilledRect(10, 6, 80, 28, 0xFF89B4FA);

    // Render active desktop windows (LCL Core Window Manager preview)
    drawWindowFrame(80, 80, 640, 420, "LCL Terminal / Core Engine", 0xFF313244);
    drawWindowFrame(400, 200, 520, 340, "LCL System Monitor", 0xFF45475A);
}

void Renderer::swapBuffers() {
    m_renderedFrames++;

    if (m_displayManager && m_displayManager->isInitialized()) {
        if (m_displayManager->getBackendType() == core::DisplayBackendType::LinuxFB) {
            uint32_t* fbPixels = m_displayManager->getFBPixelData();
            if (fbPixels) {
                size_t copyBytes = std::min(static_cast<size_t>(m_width * m_height * sizeof(uint32_t)),
                                            static_cast<size_t>(m_displayManager->getFBDevice().size));
                std::memcpy(fbPixels, m_softwareBackBuffer.data(), copyBytes);
            }
        } else if (m_usingDRMHardware && m_dumbBuffer.pixelData) {
            std::memcpy(m_dumbBuffer.pixelData, m_softwareBackBuffer.data(), m_dumbBuffer.size);
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
    if (m_dumbBuffer.pixelData) {
        munmap(m_dumbBuffer.pixelData, m_dumbBuffer.size);
        m_dumbBuffer.pixelData = nullptr;
    }
}

} // namespace lcl::render
