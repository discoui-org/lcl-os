#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lcl::core {
class EGLBackend;
}

namespace lcl::render {

struct SkiaColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{255};

    uint32_t toARGB() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
                static_cast<uint32_t>(b);
    }

    uint32_t toABGR() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
                static_cast<uint32_t>(r);
    }
};

struct SkiaRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct SkiaGradient {
    SkiaColor startColor;
    SkiaColor endColor;
    bool isVertical{true};
};

enum class SkiaBackendType {
    OpenGL_EGL,
    SoftwareRaster
};

class SkiaRenderer {
public:
    SkiaRenderer() = default;
    ~SkiaRenderer();

    // Non-copyable
    SkiaRenderer(const SkiaRenderer&) = delete;
    SkiaRenderer& operator=(const SkiaRenderer&) = delete;

    /**
     * @brief Initialize Skia rendering engine (GL hardware acceleration or CPU raster fallback).
     * @param width Surface width
     * @param height Surface height
     * @param eglBackend Optional EGL backend pointer for OpenGL GPU context
     * @param targetPixels Pointer to target pixel buffer for software rasterization
     * @return true if initialized successfully
     */
    bool initialize(uint32_t width, uint32_t height, lcl::core::EGLBackend* eglBackend = nullptr, uint32_t* targetPixels = nullptr);

    /**
     * @brief Set target buffer for software rasterization.
     */
    void setTargetPixels(uint32_t* targetPixels) { m_targetPixels = targetPixels; }

    /**
     * @brief Shutdown Skia renderer.
     */
    void shutdown();

    /**
     * @brief Begin frame drawing sequence. Clears canvas.
     */
    void beginFrame();

    /**
     * @brief End frame drawing sequence. Flushes Skia canvas commands.
     */
    void endFrame();

    // --- Skia 2D Canvas Primitives ---
    void drawBackgroundGradient(const SkiaColor& topColor, const SkiaColor& bottomColor);
    void drawRect(const SkiaRect& rect, const SkiaColor& color);
    void drawRoundedRect(const SkiaRect& rect, float radius, const SkiaColor& color, const SkiaColor& borderColor = {0,0,0,0}, float borderWidth = 0.0f);
    void drawDropShadow(const SkiaRect& rect, float radius, float blur, const SkiaColor& shadowColor);
    void drawCircle(float cx, float cy, float radius, const SkiaColor& color);
    void drawLine(float x1, float y1, float x2, float y2, const SkiaColor& color, float strokeWidth = 1.0f);
    void drawBuffer(int dstX, int dstY, int srcW, int srcH, const uint32_t* pixelData, int stridePixels = 0, float opacity = 1.0f);

    // Accessors
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    SkiaBackendType getBackendType() const { return m_backendType; }
    uint32_t* getRasterBuffer() { return m_targetPixels ? m_targetPixels : m_rasterPixels.data(); }

private:
    uint32_t m_width{0};
    uint32_t m_height{0};
    SkiaBackendType m_backendType{SkiaBackendType::SoftwareRaster};
    lcl::core::EGLBackend* m_eglBackend{nullptr};

    std::vector<uint32_t> m_rasterPixels;
    uint32_t* m_targetPixels{nullptr};
    bool m_initialized{false};
};

} // namespace lcl::render
