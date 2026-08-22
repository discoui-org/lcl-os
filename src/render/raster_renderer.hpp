#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"
#include "lcl-graphics/display_list.hpp"
#include "render/font_renderer.hpp"

#include "platform/common/graphics_context.hpp"

namespace lcl::render {

struct RasterColor {
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

struct RasterRect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

struct RasterGradient {
    RasterColor startColor;
    RasterColor endColor;
    bool isVertical{true};
};

enum class RasterBackend {
    OpenGL_EGL,
    SoftwareRaster
};

class RasterRenderer {
public:
    RasterRenderer() = default;
    ~RasterRenderer();

    // Non-copyable
    RasterRenderer(const RasterRenderer&) = delete;
    RasterRenderer& operator=(const RasterRenderer&) = delete;

    /**
     * @brief Initialize LCL raster rendering engine (GL hardware acceleration or CPU raster fallback).
     * @param width Surface width
     * @param height Surface height
     * @param eglBackend Optional EGL context for OpenGL GPU rendering
     * @param targetPixels Pointer to target pixel buffer for software rasterization
     * @return true if initialized successfully
     */
    bool initialize(uint32_t width, uint32_t height,
                    lcl::platform::IGraphicsContext* eglBackend = nullptr,
                    uint32_t* targetPixels = nullptr);

    /**
     * @brief Set target buffer and dimensions for software rasterization.
     */
    void setTargetPixels(uint32_t* targetPixels, uint32_t width = 0, uint32_t height = 0);
    /** Change the drawable content viewport without resizing the EGL backing. */
    void setFrameExtent(uint32_t width, uint32_t height);
    /** Grow the retained scene texture without changing its content viewport. */
    bool ensureFrameBackingCapacity(uint32_t width, uint32_t height);
    /** Select an externally owned GL framebuffer for one client frame. */
    void setExternalFrameTarget(uint32_t framebuffer, uint32_t texture = 0,
                                uint32_t backingWidth = 0, uint32_t backingHeight = 0);
    void clearExternalFrameTarget();
    /** Allocate one same-context RGBA texture/FBO for a widget cache. */
    bool createCachedLayerTarget(uint32_t width, uint32_t height,
                                 uint32_t& framebuffer, uint32_t& texture);
    void destroyCachedLayerTarget(uint32_t framebuffer, uint32_t texture);
    /** Temporarily redirect primitive drawing into a cached layer. */
    bool beginCachedLayerTarget(uint32_t framebuffer, uint32_t texture,
                                uint32_t width, uint32_t height,
                                uint32_t* softwarePixels,
                                float logicalOriginX, float logicalOriginY,
                                float effectiveScale);
    void endCachedLayerTarget();
    void drawCachedLayerTexture(uint32_t texture, const RasterRect& destination,
                                float opacity = 1.0f);

    /**
     * Applies a logical-pixel to raster-pixel transform to client drawing calls.
     * The compositor leaves this at 1.0; lcl-ui WindowApp sets it to its DPR.
     */
    void setDeviceScale(float scale);
    float getDeviceScale() const { return m_deviceScale; }
    /** Client canvases retain scene pixels; compositor frames remain clearing. */
    void setRetainsFrameBacking(bool enabled) { m_retainsFrameBacking = enabled; }

    /** Physical framebuffer origin for a logical content subtree. */
    void setContentOrigin(float x, float y) { m_contentOriginX = x; m_contentOriginY = y; }
    float getContentOriginX() const { return m_contentOriginX; }
    float getContentOriginY() const { return m_contentOriginY; }

    void setClipRect(const std::optional<RasterRect>& clip);
    const std::optional<RasterRect>& getClipRect() const { return m_clipRect; }

    /**
     * @brief Clear whole canvas or subregion with specific background color.
     */
    void clear(const RasterColor& color);
    void clearRect(const RasterRect& rect, const RasterColor& color);

    /**
     * @brief Shutdown LCL raster renderer.
     */
    void shutdown();

    /** Begin drawing into the retained scene backing store. */
    void beginFrame();

    /**
     * @brief End frame drawing sequence. Flushes LCL raster canvas commands.
     */
    void endFrame();

    /** Replay logical drawing commands; target DPR is applied only here. */
    void replayDisplayList(const lcl::graphics::DisplayList& displayList,
                           const lcl::graphics::RenderTarget& target,
                           const lcl::graphics::Matrix3& rootTransform = {});

    /** Draw one logical path. GPU backends keep recognized primitives on-GPU;
     * software and generic paths share the CPU raster fallback. */
    void drawPath(const lcl::graphics::Path& path,
                  const lcl::graphics::Paint& paint,
                  const lcl::graphics::Matrix3& logicalTransform = {},
                  float inheritedOpacity = 1.0f);

    // --- LCL raster 2D Canvas Primitives ---
    void drawBackgroundGradient(const RasterColor& topColor, const RasterColor& bottomColor);
    void drawRect(const RasterRect& rect, const RasterColor& color);
    void drawRoundedRect(const RasterRect& rect,
                         float radius,
                         const RasterColor& color,
                         const RasterColor& borderColor = {0,0,0,0},
                         float borderWidth = 0.0f,
                         float roundness = 2.0f);
    // Filled shape with rounded upper corners and square lower corners.
    // Title bars are shorter than twice their window radius, so a full rounded
    // rect cannot represent the outer window silhouette without a leaky patch.
    void drawTopRoundedRect(const RasterRect& rect,
                            float radius,
                            const RasterColor& color,
                            float roundness = 2.0f);
    void drawDropShadow(const RasterRect& rect, float radius, float blur, const RasterColor& shadowColor);
    void drawCircle(float cx, float cy, float radius, const RasterColor& color);
    void drawLine(float x1, float y1, float x2, float y2, const RasterColor& color, float strokeWidth = 1.0f);
    void drawString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize = 15.0f);
    /** Draw text with the packaged JetBrains Mono face. */
    void drawMonospaceString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize = 15.0f);
    /** Returns the rendered text width in the caller's logical coordinate space. */
    float measureString(const std::string& text, float fontSize = 15.0f);
    float measureMonospaceString(const std::string& text, float fontSize = 15.0f);
    /** Rasterizes one stable text layer at the current content scale. */
    bool rasterizeString(const std::string& text,
                         uint32_t fgColor,
                         float fontSize,
                         bool monospace,
                         std::vector<uint32_t>& pixels,
                         int& width,
                         int& height);
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
    /** Composite a presentation texture without quantizing animated bounds. */
    void drawBufferTransformed(float dstX,
                               float dstY,
                               int srcW,
                               int srcH,
                               const uint32_t* pixelData,
                               int stridePixels,
                               float opacity,
                               float cornerRadius,
                               float cornerRoundness,
                               bool squareTopCorners,
                               float drawWidth,
                               float drawHeight);
    void applyBackdropFilter(float dstX, float dstY, float srcW, float srcH,
                             float cornerRadius, float cornerRoundness,
                             float opacity, const std::vector<protocol::FilterOp>& filters);
    /** Imports and composites compositor-owned native textures without CPU upload. */
    uint32_t importTexture(const lcl::platform::INativeBuffer& buffer);
    uint32_t importDmaBuf(const lcl::platform::DmaBufDescriptor& descriptor);
    void releaseTexture(uint32_t texture);
    uint32_t importDmaBufTexture(const lcl::platform::INativeBuffer& buffer) {
        return importTexture(buffer);
    }
    void releaseDmaBufTexture(uint32_t texture) {
        releaseTexture(texture);
    }
    void drawDmaBufTextureTransformed(float dstX, float dstY, int srcW, int srcH,
                                      int backingW, int backingH,
                                      uint32_t texture, float opacity,
                                      float cornerRadius, float cornerRoundness,
                                      bool squareTopCorners, float drawWidth, float drawHeight);

    // Accessors
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    RasterBackend getBackendType() const { return m_backendType; }
    uint32_t* getRasterBuffer() { return m_targetPixels ? m_targetPixels : m_rasterPixels.data(); }

private:
    struct CachedLayerTargetState {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t* targetPixels{nullptr};
        float contentOriginX{0.0f};
        float contentOriginY{0.0f};
        float deviceScale{1.0f};
        std::optional<RasterRect> clip;
        uint32_t externalFrameFBO{0};
        uint32_t externalFrameTexture{0};
        uint32_t externalBackingWidth{0};
        uint32_t externalBackingHeight{0};
    };

    bool initGLShader();
    RasterRect scaleRect(const RasterRect& rect) const;
    int scaleCoord(int value) const;
    int scaleLength(int value) const;
    bool ensureFont(float logicalFontSize);
    bool ensureMonospaceFont(float logicalFontSize);
    void drawBufferRaw(float dstX,
                       float dstY,
                       int srcW,
                       int srcH,
                       const uint32_t* pixelData,
                       int stridePixels,
                       float opacity,
                       float cornerRadius,
                       float cornerRoundness,
                       bool squareTopCorners,
                       bool squareBottomCorners,
                       float drawWidth,
                       float drawHeight);

    uint32_t m_width{0};
    uint32_t m_height{0};
    RasterBackend m_backendType{RasterBackend::SoftwareRaster};
    lcl::platform::IGraphicsContext* m_eglBackend{nullptr};

    std::vector<uint32_t> m_rasterPixels;
    uint32_t* m_targetPixels{nullptr};
    FontRenderer m_fontRenderer;
    FontRenderer m_monospaceFontRenderer;
    float m_deviceScale{1.0f};
    float m_contentOriginX{0.0f};
    float m_contentOriginY{0.0f};
    bool m_initialized{false};
    bool m_retainsFrameBacking{false};

    uint32_t m_glTexture{0};
    uint32_t m_glProgram{0};
    int32_t m_aPosLoc{-1};
    int32_t m_aTexLoc{-1};
    int32_t m_uTextureLoc{-1};
    int32_t m_uOpacityLoc{-1};

    // GPU FBO & Shader Handles for Backdrop Filters & Scene Compositing
    bool m_glFBOReady{false};
    uint32_t m_glFBO[2]{0, 0};
    uint32_t m_glFBOTexture[2]{0, 0};
    uint32_t m_glFBOCapacityWidth{0};
    uint32_t m_glFBOCapacityHeight{0};

    uint32_t m_glSceneFBO{0};
    uint32_t m_glSceneTexture{0};
    uint32_t m_glSceneCapacityWidth{0};
    uint32_t m_glSceneCapacityHeight{0};
    uint32_t m_glExternalFrameFBO{0};
    uint32_t m_glExternalFrameTexture{0};
    uint32_t m_glExternalBackingWidth{0};
    uint32_t m_glExternalBackingHeight{0};
    // Rotating DMA-BUF output is separate from the authoritative retained
    // scene FBO. A completed scene is copied here once per submitted frame.
    uint32_t m_glOutputFrameFBO{0};
    std::optional<CachedLayerTargetState> m_cachedLayerTargetState;

    uint32_t m_glBlurProgram{0};
    int32_t m_aBlurPosLoc{-1};
    int32_t m_aBlurTexLoc{-1};
    int32_t m_uBlurTextureLoc{-1};
    int32_t m_uBlurDirLoc{-1};
    int32_t m_uBlurSigmaLoc{-1};
    int32_t m_uBlurRadiusLoc{-1};
    int32_t m_uBlurSizeLoc{-1};
    int32_t m_uBlurCornerRadiusLoc{-1};
    int32_t m_uBlurRoundnessLoc{-1};
    int32_t m_uBlurInputScaleLoc{-1};

    uint32_t m_glColorMatrixProgram{0};
    int32_t m_aColorPosLoc{-1};
    int32_t m_aColorTexLoc{-1};
    int32_t m_uColorTextureLoc{-1};
    int32_t m_uColorMatrixLoc{-1};
    int32_t m_uColorOffsetLoc{-1};
    int32_t m_uColorInputScaleLoc{-1};

    uint32_t m_glMaskProgram{0};
    int32_t m_aMaskPosLoc{-1};
    int32_t m_aMaskTexLoc{-1};
    int32_t m_uMaskTextureLoc{-1};
    int32_t m_uMaskSizeLoc{-1};
    int32_t m_uMaskRadiusLoc{-1};
    int32_t m_uMaskRoundnessLoc{-1};
    int32_t m_uMaskOpacityLoc{-1};
    int32_t m_uMaskSquareTopCornersLoc{-1};
    int32_t m_uMaskSampleOffsetLoc{-1};
    int32_t m_uMaskSampleScaleLoc{-1};

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
    int32_t m_uRefractCaptureSizeLoc{-1};
    int32_t m_uRefractEffectOffsetLoc{-1};
    int32_t m_uRefractRadiusLoc{-1};
    int32_t m_uRefractRoundnessLoc{-1};
    int32_t m_uRefractInputScaleLoc{-1};
    int32_t m_uRefractLogicalToPassScaleLoc{-1};

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
    void drawTextureQuad(uint32_t textureId, float x, float y, float w, float h,
                         float opacity = 1.0f, float uMax = 1.0f, float vMax = 1.0f);
    void drawMaskedTextureQuad(uint32_t textureId, float x, float y, float w, float h,
                               float cornerRadius, float cornerRoundness, float opacity,
                               bool squareTopCorners = false,
                               float uScale = 1.0f, float vScale = 1.0f,
                               float uOffset = 0.0f, float vOffset = 0.0f);
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
                            const RasterColor& fill,
                            const RasterColor& border);
    uint32_t activeSceneFBO() const {
        return m_glExternalFrameFBO ? m_glExternalFrameFBO : m_glSceneFBO;
    }
    uint32_t activeSceneTexture() const {
        return m_glExternalFrameTexture ? m_glExternalFrameTexture : m_glSceneTexture;
    }

    std::optional<RasterRect> m_clipRect;
    void applyScissorState();
};

} // namespace lcl::render
