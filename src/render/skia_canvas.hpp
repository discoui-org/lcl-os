#pragma once

#include <memory>

#include "lcl-ui/core/canvas.hpp"
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

    void drawRect(const lcl::ui::Rect& rect, lcl::ui::Color color) override;
    void drawRoundedRect(const lcl::ui::Rect& rect, float radius, lcl::ui::Color color,
                         lcl::ui::Color border, float borderWidth, float roundness) override;
    void drawTopRoundedRect(const lcl::ui::Rect& rect, float radius, lcl::ui::Color color,
                            float roundness) override;
    void drawText(float x, float y, const std::string& text, lcl::ui::Color color,
                  float fontSize) override;
    float measureText(const std::string& text, float fontSize) override;
    void drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                    const uint32_t* pixels, int stridePixels, float opacity,
                    float cornerRadius, float cornerRoundness,
                    bool squareTopCorners, int drawWidth, int drawHeight) override;

private:
    static SkiaColor toSkia(lcl::ui::Color color);
    SkiaRenderer& renderer() { return *m_renderer; }

    std::unique_ptr<SkiaRenderer> m_ownedRenderer;
    SkiaRenderer* m_renderer{nullptr};
};

std::unique_ptr<lcl::ui::Canvas> makeSkiaCanvas();

} // namespace lcl::render
