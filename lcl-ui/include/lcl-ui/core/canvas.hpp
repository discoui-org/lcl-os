#pragma once

#include <cstdint>
#include <string>

#include "lcl-ui/core/rect.hpp"

namespace lcl::ui {

struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0}; // Default completely transparent (unstyled baseline)
};

struct AffineTransform {
    float a{1.0f};
    float b{0.0f};
    float c{0.0f};
    float d{1.0f};
    float tx{0.0f};
    float ty{0.0f};
};

/** Selects the text face without exposing a renderer implementation to widgets. */
enum class FontFamily : uint8_t {
    Interface,
    Monospace,
};

/**
 * Backend-neutral drawing and frame-target contract used by lcl-ui widgets.
 * Coordinates are logical pixels; the selected Canvas owns any raster mapping.
 */
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) = 0;
    virtual void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) = 0;
    virtual void setContentScale(float scale) = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual uint32_t* rasterBuffer() = 0;

    // Backend-neutral presentation-layer primitives. Default implementations
    // preserve compatibility for minimal/test canvases that do not transform.
    virtual void saveState() {}
    virtual void restoreState() {}
    virtual void clipRect(const Rect&) {}
    virtual void concatTransform(const AffineTransform&) {}
    virtual void beginLayer(float) {}
    virtual void endLayer() {}

    virtual void drawRect(const Rect& rect, Color color) = 0;
    virtual void drawRoundedRect(const Rect& rect, float radius, Color color,
                                 Color border, float borderWidth, float roundness) = 0;
    virtual void drawTopRoundedRect(const Rect& rect, float radius, Color color,
                                    float roundness) = 0;
    virtual void drawText(float x, float y, const std::string& text, Color color,
                          float fontSize, FontFamily family = FontFamily::Interface) = 0;
    virtual float measureText(const std::string& text, float fontSize,
                              FontFamily family = FontFamily::Interface) = 0;
    virtual void drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners, int drawWidth, int drawHeight) = 0;
};

} // namespace lcl::ui
