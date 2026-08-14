#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lcl-ui/core/canvas.hpp"
#include "render/client_egl_context.hpp"
#include "render/skia_renderer.hpp"

namespace lcl::render {

/** Canvas adapter for the existing SkiaRenderer implementation. */
class SkiaCanvas final : public lcl::ui::Canvas {
public:
    SkiaCanvas();
    explicit SkiaCanvas(SkiaRenderer& renderer);

    bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) override;
    void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) override;
    void setContentScale(float scale) override;
    void beginFrame() override;
    void endFrame() override;
    uint32_t* rasterBuffer() override;
    bool isDmaBufFrameActive() const override;
    void setDmaBufTransportEnabled(bool enabled) override;
    bool hasDmaBufTransport() const override;
    bool configureDmaBufFrame(uint32_t contentWidth, uint32_t contentHeight,
                              uint32_t backingWidth, uint32_t backingHeight) override;
    bool isDmaBufFrameBlocked() const override;
    std::optional<lcl::ui::DmaBufFrame> takeDmaBufFrame() override;
    void cancelDmaBufFrame(uint32_t bufferId) override;
    void releaseDmaBufFrame(uint32_t bufferId) override;

    void saveState() override;
    void restoreState() override;
    void clipRect(const lcl::ui::Rect& rect) override;
    void concatTransform(const lcl::ui::AffineTransform& transform) override;
    void beginLayer(float opacity) override;
    void endLayer() override;

    void drawRect(const lcl::ui::Rect& rect, lcl::ui::Color color) override;
    void drawRoundedRect(const lcl::ui::Rect& rect, float radius, lcl::ui::Color color,
                         lcl::ui::Color border, float borderWidth, float roundness) override;
    void drawTopRoundedRect(const lcl::ui::Rect& rect, float radius, lcl::ui::Color color,
                            float roundness) override;
    void drawText(float x, float y, const std::string& text, lcl::ui::Color color,
                  float fontSize, lcl::ui::FontFamily family) override;
    void drawRasterizedText(float x, float y, const std::string& text,
                            lcl::ui::Color color, float fontSize,
                            lcl::ui::FontFamily family) override;
    float measureText(const std::string& text, float fontSize,
                      lcl::ui::FontFamily family) override;
    void drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                    const uint32_t* pixels, int stridePixels, float opacity,
                    float cornerRadius, float cornerRoundness,
                    bool squareTopCorners, int drawWidth, int drawHeight) override;

private:
    struct CanvasState {
        lcl::ui::AffineTransform transform{};
        float opacity{1.0f};
        std::optional<lcl::ui::Rect> clip{};
    };

    struct TextLayer {
        std::string text;
        uint32_t argb{0};
        float fontSize{0.0f};
        float contentScale{1.0f};
        lcl::ui::FontFamily family{lcl::ui::FontFamily::Interface};
        int width{0};
        int height{0};
        uint64_t lastUse{0};
        std::vector<uint32_t> pixels;
    };

    static SkiaColor toSkia(lcl::ui::Color color);
    lcl::ui::Rect mapRect(const lcl::ui::Rect& rect) const;
    std::pair<float, float> mapPoint(float x, float y) const;
    lcl::ui::Color mapColor(lcl::ui::Color color) const;
    bool applyClip(lcl::ui::Rect& rect) const;
    SkiaRenderer& renderer() { return *m_renderer; }

    std::unique_ptr<ClientEGLContext> m_clientEglContext;
    // Keep the EGL context alive until after the renderer has released its GL
    // objects (members are destroyed in reverse declaration order).
    std::unique_ptr<SkiaRenderer> m_ownedRenderer;
    SkiaRenderer* m_renderer{nullptr};
    CanvasState m_state{};
    std::vector<CanvasState> m_stack;
    std::vector<float> m_layerOpacityStack;
    std::vector<TextLayer> m_textLayers;
    uint64_t m_textLayerUseCounter{0};
    float m_contentScale{1.0f};
    bool m_dmaBufFrameActive{false};
    bool m_dmaBufFrameBlocked{false};
    bool m_dmaBufTransportEnabled{false};
    uint32_t m_dmaBufContentWidth{0};
    uint32_t m_dmaBufContentHeight{0};
};

std::unique_ptr<lcl::ui::Canvas> makeSkiaCanvas();

} // namespace lcl::render
