#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"
#include "render/font_renderer.hpp"

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
     * @brief Set target buffer and dimensions for software rasterization.
     */
    void setTargetPixels(uint32_t* targetPixels, uint32_t width = 0, uint32_t height = 0) {
        m_targetPixels = targetPixels;
        if (width > 0) m_width = width;
        if (height > 0) m_height = height;
    }

    /**
     * Applies a logical-pixel to raster-pixel transform to client drawing calls.
     * The compositor leaves this at 1.0; lcl-ui WindowApp sets it to its DPR.
     */
    void setContentScale(float scale);
    float getContentScale() const { return m_contentScale; }

    /** Physical framebuffer origin for a logical content subtree. */
    void setContentOrigin(float x, float y) { m_contentOriginX = x; m_contentOriginY = y; }
    float getContentOriginX() const { return m_contentOriginX; }
    float getContentOriginY() const { return m_contentOriginY; }

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
    void drawRoundedRect(const SkiaRect& rect,
                         float radius,
                         const SkiaColor& color,
                         const SkiaColor& borderColor = {0,0,0,0},
                         float borderWidth = 0.0f,
                         float roundness = 2.0f);
    // Filled shape with rounded upper corners and square lower corners.
    // Title bars are shorter than twice their window radius, so a full rounded
    // rect cannot represent the outer window silhouette without a leaky patch.
    void drawTopRoundedRect(const SkiaRect& rect,
                            float radius,
                            const SkiaColor& color,
                            float roundness = 2.0f);
    void drawDropShadow(const SkiaRect& rect, float radius, float blur, const SkiaColor& shadowColor);
    void drawCircle(float cx, float cy, float radius, const SkiaColor& color);
    void drawLine(float x1, float y1, float x2, float y2, const SkiaColor& color, float strokeWidth = 1.0f);
    void drawString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize = 15.0f);
    /** Draw text with the packaged JetBrains Mono face. */
    void drawMonospaceString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize = 15.0f);
    /** Returns the rendered text width in the caller's logical coordinate space. */
    float measureString(const std::string& text, float fontSize = 15.0f);
    float measureMonospaceString(const std::string& text, float fontSize = 15.0f);
    void drawBuffer(int dstX,
                    int dstY,
                    int srcW,
                    int srcH,
                    const uint32_t* pixelData,
                    int stridePixels = 0,
                    float opacity = 1.0f,
                    float cornerRadius = 0.0f,
                    float cornerRoundness = 2.0f,
                    bool squareTopCorners = false,
                    int drawWidth = 0,
                    int drawHeight = 0);
    void applyBackdropFilter(int dstX, int dstY, int srcW, int srcH, float cornerRadius, float opacity, const std::vector<protocol::FilterOp>& filters);

    // Accessors
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    SkiaBackendType getBackendType() const { return m_backendType; }
    uint32_t* getRasterBuffer() { return m_targetPixels ? m_targetPixels : m_rasterPixels.data(); }

private:
    bool initGLShader();
    SkiaRect scaleRect(const SkiaRect& rect) const;
    int scaleCoord(int value) const;
    int scaleLength(int value) const;
    bool ensureFont(float logicalFontSize);
    bool ensureMonospaceFont(float logicalFontSize);
    void drawBufferRaw(int dstX,
                       int dstY,
                       int srcW,
                       int srcH,
                       const uint32_t* pixelData,
                       int stridePixels,
                       float opacity,
                       float cornerRadius,
                       float cornerRoundness,
                       bool squareTopCorners,
                       bool squareBottomCorners,
                       int drawWidth,
                       int drawHeight);

    uint32_t m_width{0};
    uint32_t m_height{0};
    SkiaBackendType m_backendType{SkiaBackendType::SoftwareRaster};
    lcl::core::EGLBackend* m_eglBackend{nullptr};

    std::vector<uint32_t> m_rasterPixels;
    uint32_t* m_targetPixels{nullptr};
    FontRenderer m_fontRenderer;
    FontRenderer m_monospaceFontRenderer;
    float m_contentScale{1.0f};
    float m_contentOriginX{0.0f};
    float m_contentOriginY{0.0f};
    bool m_initialized{false};

    uint32_t m_glTexture{0};
    uint32_t m_glProgram{0};
    int32_t m_aPosLoc{-1};
    int32_t m_aTexLoc{-1};
    int32_t m_uTextureLoc{-1};

    // GPU FBO & Shader Handles for Backdrop Filters & Scene Compositing
    bool m_glFBOReady{false};
    uint32_t m_glFBO[2]{0, 0};
    uint32_t m_glFBOTexture[2]{0, 0};

    uint32_t m_glSceneFBO{0};
    uint32_t m_glSceneTexture{0};

    uint32_t m_glBlurProgram{0};
    int32_t m_aBlurPosLoc{-1};
    int32_t m_aBlurTexLoc{-1};
    int32_t m_uBlurTextureLoc{-1};
    int32_t m_uBlurDirLoc{-1};
    int32_t m_uBlurSigmaLoc{-1};
    int32_t m_uBlurRadiusLoc{-1};

    uint32_t m_glColorMatrixProgram{0};
    int32_t m_aColorPosLoc{-1};
    int32_t m_aColorTexLoc{-1};
    int32_t m_uColorTextureLoc{-1};
    int32_t m_uColorMatrixLoc{-1};
    int32_t m_uColorOffsetLoc{-1};

    uint32_t m_glMaskProgram{0};
    int32_t m_aMaskPosLoc{-1};
    int32_t m_aMaskTexLoc{-1};
    int32_t m_uMaskTextureLoc{-1};
    int32_t m_uMaskSizeLoc{-1};
    int32_t m_uMaskRadiusLoc{-1};
    int32_t m_uMaskRoundnessLoc{-1};
    int32_t m_uMaskOpacityLoc{-1};

    uint32_t m_glMaskBgraProgram{0};
    int32_t m_aMaskBgraPosLoc{-1};
    int32_t m_aMaskBgraTexLoc{-1};
    int32_t m_uMaskBgraTextureLoc{-1};
    int32_t m_uMaskBgraSizeLoc{-1};
    int32_t m_uMaskBgraCornerRadiiLoc{-1};
    int32_t m_uMaskBgraRoundnessLoc{-1};
    int32_t m_uMaskBgraOpacityLoc{-1};
    int32_t m_uMaskBgraTopOnlyLoc{-1};

    uint32_t m_glRoundRectProgram{0};
    int32_t m_aRoundRectPosLoc{-1};
    int32_t m_aRoundRectTexLoc{-1};
    int32_t m_uRoundRectSizeLoc{-1};
    int32_t m_uRoundRectRadiusLoc{-1};
    int32_t m_uRoundRectRoundnessLoc{-1};
    int32_t m_uRoundRectBorderWidthLoc{-1};
    int32_t m_uRoundRectFillColorLoc{-1};
    int32_t m_uRoundRectBorderColorLoc{-1};

    uint32_t m_glRefractionProgram{0};
    int32_t m_aRefractPosLoc{-1};
    int32_t m_aRefractTexLoc{-1};
    int32_t m_uRefractTextureLoc{-1};
    int32_t m_uRefractInvSizeLoc{-1};
    int32_t m_uRefractThicknessLoc{-1};
    int32_t m_uRefractFactorLoc{-1};
    int32_t m_uRefractDispersionLoc{-1};
    int32_t m_uRefractSizeLoc{-1};
    int32_t m_uRefractRadiusLoc{-1};

    // GPU BGRA Surface Compositing Handles
    uint32_t m_glClientTexture{0};
    int32_t m_glClientTextureWidth{0};
    int32_t m_glClientTextureHeight{0};
    uint32_t m_glBgraProgram{0};
    int32_t m_aBgraPosLoc{-1};
    int32_t m_aBgraTexLoc{-1};
    int32_t m_uBgraTextureLoc{-1};
    int32_t m_uBgraOpacityLoc{-1};

    // Helper for rendering textured quads on GPU
    void drawTextureQuad(uint32_t textureId, float x, float y, float w, float h, float opacity = 1.0f);
    void drawMaskedTextureQuad(uint32_t textureId, float x, float y, float w, float h, float cornerRadius, float cornerRoundness, float opacity);
    void drawMaskedBgraTextureQuad(uint32_t textureId,
                                   float x,
                                   float y,
                                   float w,
                                   float h,
                                   float cornerRadius,
                                   float cornerRoundness,
                                   float opacity,
                                   bool squareTopCorners = false,
                                   bool squareBottomCorners = false);
    void drawBgraTextureQuad(uint32_t textureId, float x, float y, float w, float h, float opacity = 1.0f);
    void drawGpuRoundedRect(float x,
                            float y,
                            float w,
                            float h,
                            float radius,
                            float roundness,
                            float borderWidth,
                            const SkiaColor& fill,
                            const SkiaColor& border);
};

} // namespace lcl::render
