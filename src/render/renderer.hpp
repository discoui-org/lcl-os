#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#include "platform/common/graphics_context.hpp"
#include "platform/common/display_backend.hpp"
#include "render/window_manager.hpp"
#include "render/render_types.hpp"
#include "render/font_renderer.hpp"
#include "render/skia_renderer.hpp"

namespace lcl::render {

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
     * @brief Initialize renderer with dimensions and platform interfaces.
     * @param width Viewport width
     * @param height Viewport height
     * @param graphicsContext Optional platform graphics context (EGL/GLES)
     * @param displayBackend Optional platform display backend
     * @param fbPixels Optional raw framebuffer pixel memory for software raster mode
     * @return true if initialized, false otherwise
     */
    bool initialize(uint32_t width = 1024, uint32_t height = 768,
                    platform::IGraphicsContext* graphicsContext = nullptr,
                    platform::IDisplayBackend* displayBackend = nullptr,
                    uint32_t* fbPixels = nullptr);

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
    void renderWindowContent(const Window& win, const WindowRenderContent* content);

    /**
     * @brief Present back buffer onto display CRTC or log virtual frame.
     */
    void swapBuffers();

    bool isInitialized() const { return m_initialized; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint64_t getRenderedFrames() const { return m_renderedFrames; }
    SkiaRenderer* getSkiaRenderer() { return &m_skiaRenderer; }

private:
    // --- Modular render passes (called by renderDesktop) ---
    void renderBackground();
    void renderTaskbar();

    platform::IGraphicsContext* m_graphicsContext{nullptr};
    platform::IDisplayBackend* m_displayBackend{nullptr};
    uint32_t* m_fbPixels{nullptr};

    FontRenderer m_fontRenderer;
    SkiaRenderer m_skiaRenderer;
    uint32_t m_width{1024};
    uint32_t m_height{768};
    bool m_initialized{false};

    std::vector<uint32_t> m_softwareBackBuffer;
    uint64_t m_renderedFrames{0};
};

} // namespace lcl::render
