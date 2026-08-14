#pragma once

#include <cstdint>
#include <optional>
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

/**
 * An optional, single-plane DMA-BUF frame exported by a Canvas backend.
 * lcl-ui owns only the protocol-neutral metadata; GBM/EGL stay in the backend.
 * The exported fd is consumed by the socket send operation.
 */
struct DmaBufFrame {
    uint32_t bufferId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    int fd{-1};
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

    /** True only while this frame is rendered directly into a DMA-BUF. */
    virtual bool isDmaBufFrameActive() const { return false; }
    /** WindowApp enables DMA-BUF only after a compatible compositor connection. */
    virtual void setDmaBufTransportEnabled(bool) {}
    /** True when this backend can deliver frames through its DMA-BUF pool. */
    virtual bool hasDmaBufTransport() const { return false; }
    /** True when every pool slot is still owned by the compositor. */
    virtual bool isDmaBufFrameBlocked() const { return false; }
    /** Exports the completed frame. Empty preserves the existing SHM commit path. */
    virtual std::optional<DmaBufFrame> takeDmaBufFrame() { return std::nullopt; }
    /** Cancels an unsent export so its pool slot can be reused. */
    virtual void cancelDmaBufFrame(uint32_t) {}
    /** Releases a compositor-owned pool slot after ReleaseDmaBuf. */
    virtual void releaseDmaBufFrame(uint32_t) {}

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
    // Draws text from a stable raster while an ancestor presentation transform
    // is moving. Backends without a raster cache retain correct behavior by
    // falling back to ordinary text drawing.
    virtual void drawRasterizedText(float x, float y, const std::string& text,
                                    Color color, float fontSize,
                                    FontFamily family = FontFamily::Interface) {
        drawText(x, y, text, color, fontSize, family);
    }
    virtual float measureText(const std::string& text, float fontSize,
                              FontFamily family = FontFamily::Interface) = 0;
    virtual void drawBuffer(int dstX, int dstY, int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners, int drawWidth, int drawHeight) = 0;
};

} // namespace lcl::ui
