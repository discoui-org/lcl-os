#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lcl-graphics/canvas.hpp"
#include "render/client_egl_context.hpp"
#include "render/raster_renderer.hpp"

namespace lcl::render {

/** Canvas adapter for the existing RasterRenderer implementation. */
class RasterCanvas final : public lcl::graphics::Canvas {
public:
    RasterCanvas();
    explicit RasterCanvas(RasterRenderer& renderer);
    ~RasterCanvas() override;

    bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) override;
    void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) override;
    void setRenderTarget(const lcl::graphics::RenderTarget& target) override;
    const lcl::graphics::RenderTarget& renderTarget() const override { return m_renderTarget; }
    void beginFrame() override;
    void endFrame() override;
    uint32_t* rasterBuffer() override;
    bool isDmaBufFrameActive() const override;
    void setDmaBufTransportEnabled(bool enabled) override;
    bool hasDmaBufTransport() const override;
    bool configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                              uint32_t backingWidth, uint32_t backingHeight) override;
    bool isDmaBufFrameBlocked() const override;
    std::optional<lcl::graphics::DmaBufFrame> takeDmaBufFrame() override;
    void cancelDmaBufFrame(uint32_t bufferId) override;
    void releaseDmaBufFrame(uint32_t bufferId) override;

    void saveState() override;
    void restoreState() override;
    void clipRect(const lcl::graphics::RectF& rect) override;
    void clipPath(const lcl::graphics::Path& path,
                  lcl::graphics::FillRule fillRule) override;
    void concatTransform(const lcl::graphics::Matrix3& transform) override;
    void beginLayer(float opacity) override;
    void endLayer() override;
    bool beginCachedLayer(CachedLayerId id, const lcl::graphics::RectF& sourceBounds) override;
    bool beginCachedLayerUpdate(
        CachedLayerId id, const lcl::graphics::RectF& sourceBounds,
        const lcl::graphics::RectF& updateBounds) override;
    void endCachedLayer() override;
    bool drawCachedLayer(CachedLayerId id, const lcl::graphics::RectF& destination,
                         float opacity = 1.0f) override;

    void clearRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) override;
    void drawPath(const lcl::graphics::Path& path,
                  const lcl::graphics::Paint& paint) override;
    void drawRect(const lcl::graphics::RectF& rect, lcl::graphics::Color color) override;
    void drawRoundedRect(const lcl::graphics::RectF& rect, float radius, lcl::graphics::Color color,
                         lcl::graphics::Color border, float borderWidth, float roundness) override;
    void drawTopRoundedRect(const lcl::graphics::RectF& rect, float radius, lcl::graphics::Color color,
                            float roundness) override;
    void drawText(float x, float y, const std::string& text, lcl::graphics::Color color,
                  float fontSize, lcl::graphics::FontFamily family) override;
    void drawRasterizedText(float x, float y, const std::string& text,
                            lcl::graphics::Color color, float fontSize,
                            lcl::graphics::FontFamily family) override;
    float measureText(const std::string& text, float fontSize,
                      lcl::graphics::FontFamily family) override;
    void drawBuffer(const lcl::graphics::RectF& destination,
                    int srcWidth, int srcHeight,
                    const uint32_t* pixels, int stridePixels, float opacity,
                    float cornerRadius, float cornerRoundness,
                    bool squareTopCorners) override;

    const lcl::graphics::DisplayList& lastDisplayList() const noexcept {
        return m_lastDisplayList;
    }

private:
    struct CanvasState {
        lcl::graphics::Matrix3 transform{};
        float opacity{1.0f};
        std::optional<lcl::graphics::RectF> clip{};
    };

    void clearCachedLayers();
    bool beginCachedLayerInternal(
        CachedLayerId id, const lcl::graphics::RectF& sourceBounds,
        std::optional<lcl::graphics::RectF> updateBounds);
    RasterRenderer& renderer() { return *m_renderer; }

    std::unique_ptr<ClientEGLContext> m_clientEglContext;
    // Keep the EGL context alive until after the renderer has released its GL
    // objects (members are destroyed in reverse declaration order).
    std::unique_ptr<RasterRenderer> m_ownedRenderer;
    RasterRenderer* m_renderer{nullptr};
    CanvasState m_state{};
    std::vector<CanvasState> m_stack;
    std::vector<float> m_layerOpacityStack;
    std::optional<CanvasState> m_cachedLayerCanvasState;
    lcl::graphics::RenderTarget m_renderTarget{};
    lcl::graphics::DisplayListBuilder m_displayListBuilder;
    lcl::graphics::DisplayList m_lastDisplayList;
    bool m_dmaBufFrameActive{false};
    bool m_dmaBufFrameBlocked{false};
    bool m_dmaBufTransportEnabled{false};
    uint32_t m_dmaBufContentWidth{0};
    uint32_t m_dmaBufContentHeight{0};
};

std::unique_ptr<lcl::graphics::Canvas> makeRasterCanvas();

} // namespace lcl::render
