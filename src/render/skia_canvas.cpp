#include "render/skia_canvas.hpp"

namespace lcl::render {

SkiaCanvas::SkiaCanvas()
    : m_ownedRenderer(std::make_unique<SkiaRenderer>()), m_renderer(m_ownedRenderer.get()) {}

SkiaCanvas::SkiaCanvas(SkiaRenderer& renderer) : m_renderer(&renderer) {}

SkiaColor SkiaCanvas::toSkia(lcl::ui::Color color) {
    return {color.r, color.g, color.b, color.a};
}

bool SkiaCanvas::initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) {
    return renderer().initialize(width, height, nullptr, targetPixels);
}

void SkiaCanvas::setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) {
    renderer().setTargetPixels(targetPixels, width, height);
}

void SkiaCanvas::setContentScale(float scale) { renderer().setContentScale(scale); }
void SkiaCanvas::beginFrame() { renderer().beginFrame(); }
void SkiaCanvas::endFrame() { renderer().endFrame(); }
uint32_t* SkiaCanvas::rasterBuffer() { return renderer().getRasterBuffer(); }

void SkiaCanvas::drawRect(const lcl::ui::Rect& rect, lcl::ui::Color color) {
    renderer().drawRect({rect.x, rect.y, rect.width, rect.height}, toSkia(color));
}

void SkiaCanvas::drawRoundedRect(const lcl::ui::Rect& rect, float radius,
                                 lcl::ui::Color color, lcl::ui::Color border,
                                 float borderWidth, float roundness) {
    renderer().drawRoundedRect({rect.x, rect.y, rect.width, rect.height}, radius,
                               toSkia(color), toSkia(border), borderWidth, roundness);
}

void SkiaCanvas::drawTopRoundedRect(const lcl::ui::Rect& rect, float radius,
                                    lcl::ui::Color color, float roundness) {
    renderer().drawTopRoundedRect({rect.x, rect.y, rect.width, rect.height}, radius,
                                  toSkia(color), roundness);
}

void SkiaCanvas::drawText(float x, float y, const std::string& text,
                          lcl::ui::Color color, float fontSize) {
    const uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                          (static_cast<uint32_t>(color.r) << 16) |
                          (static_cast<uint32_t>(color.g) << 8) |
                          static_cast<uint32_t>(color.b);
    renderer().drawString(static_cast<int>(x), static_cast<int>(y), text, argb, fontSize);
}

float SkiaCanvas::measureText(const std::string& text, float fontSize) {
    return renderer().measureString(text, fontSize);
}

void SkiaCanvas::drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners, int drawWidth, int drawHeight) {
    renderer().drawBuffer(dstX, dstY, srcWidth, srcHeight, pixels, stridePixels, opacity,
                          cornerRadius, cornerRoundness, squareTopCorners, drawWidth, drawHeight);
}

std::unique_ptr<lcl::ui::Canvas> makeSkiaCanvas() {
    return std::make_unique<SkiaCanvas>();
}

} // namespace lcl::render
