#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "core/display/display_manager.hpp"
#include "render/window_manager.hpp"

namespace lcl::render {

struct WindowRenderContent {
    uint32_t windowId{0};
    std::vector<std::string> lines;
};

struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};

    static Color FromARGB(uint32_t argb) {
        return Color{
            static_cast<uint8_t>((argb >> 16) & 0xFF),
            static_cast<uint8_t>((argb >> 8) & 0xFF),
            static_cast<uint8_t>(argb & 0xFF),
            static_cast<uint8_t>((argb >> 24) & 0xFF)
        };
    }

    uint32_t toARGB() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
               static_cast<uint32_t>(b);
    }
};

struct Framebuffer {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t pitch{0};
    uint32_t handle{0};
    uint32_t fbId{0};
    uint64_t size{0};
    uint32_t* pixelData{nullptr};
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    // Non-copyable
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Moveable
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    /**
     * @brief Initialize renderer using DisplayManager context or fallback canvas.
     * @param displayManager Pointer to initialized DisplayManager instance
     * @return true if initialized, false otherwise
     */
    bool initialize(core::DisplayManager* displayManager);

    /**
     * @brief Release framebuffer and rendering resources.
     */
    void shutdown();

    // Drawing primitives
    void clear(uint32_t argbColor);
    void drawPixel(int x, int y, uint32_t argbColor);
    void drawRect(int x, int y, int width, int height, uint32_t argbColor);
    void drawFilledRect(int x, int y, int width, int height, uint32_t argbColor);

    // Monospace Font primitives & Clipped drawing
    void drawChar(int x, int y, char c, uint32_t fgColor);
    void drawString(int x, int y, const std::string& text, uint32_t fgColor);
    void drawCharClipped(int x, int y, char c, uint32_t fgColor, int minX, int minY, int maxX, int maxY);
    void drawStringClipped(int x, int y, const std::string& text, uint32_t fgColor, int minX, int minY, int maxX, int maxY);

    // High-level LCL UI primitives
    void drawCursor(int mouseX, int mouseY);
    void renderLCLDesktopShell(const std::string& statusMessage);
    void renderDesktop(const WindowManager& windowManager, const std::vector<WindowRenderContent>& windowContents);
    void drawWindowFrame(int x, int y, int width, int height, const std::string& title, uint32_t headerColor);

    /**
     * @brief Present back buffer onto DRM display CRTC or log virtual frame.
     */
    void swapBuffers();

    bool isInitialized() const { return m_initialized; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint64_t getRenderedFrames() const { return m_renderedFrames; }

private:
    bool createDumbBuffer();
    void destroyDumbBuffer();

    core::DisplayManager* m_displayManager{nullptr};
    uint32_t m_width{1024};
    uint32_t m_height{768};
    bool m_initialized{false};
    bool m_usingDRMHardware{false};

    Framebuffer m_dumbBuffer;
    std::vector<uint32_t> m_softwareBackBuffer;
    uint64_t m_renderedFrames{0};
};

} // namespace lcl::render
