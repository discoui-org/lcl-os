#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lcl-graphics/display_list.hpp"
#include "lcl-graphics/font.hpp"

namespace lcl::graphics {

/** Immutable image pixels identified independently from their memory address. */
struct ImageResourceView {
    uint64_t id{0};
    uint64_t contentRevision{0};
    int width{0};
    int height{0};
    const uint32_t* pixels{nullptr};
    int stridePixels{0};
    bool opaque{false};
};

struct DisplayListFrame {
    DisplayList displayList;
    std::vector<ImageResourceView> imageResources;
};

enum class NativeBufferTransport : uint8_t {
    DmaBuf = 0,
    AndroidHardwareBufferV1 = 1,
};

/** A GPU-native frame whose color channels are premultiplied by alpha. */
struct DmaBufFrame {
    uint32_t bufferId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t backingWidth{0};
    uint32_t backingHeight{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    int fd{-1};
    NativeBufferTransport transport{NativeBufferTransport::DmaBuf};
    int acquireFenceFd{-1};
};

class Canvas {
public:
    using CachedLayerId = uint64_t;
    virtual ~Canvas() = default;

    virtual bool initialize(uint32_t width, uint32_t height, uint32_t* targetPixels) = 0;
    virtual void setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) = 0;
    virtual void setRenderTarget(const RenderTarget& target) = 0;
    virtual const RenderTarget& renderTarget() const = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual uint32_t* rasterBuffer() = 0;

    virtual bool usesDisplayListTransport() const { return false; }
    virtual std::optional<DisplayListFrame> takeDisplayListFrame() {
        return std::nullopt;
    }

    virtual bool isDmaBufFrameActive() const { return false; }
    virtual void setDmaBufTransportEnabled(bool) {}
    virtual bool hasDmaBufTransport() const { return false; }
    virtual bool configureDmaBufFrame(uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
    virtual bool isDmaBufFrameBlocked() const { return false; }
    virtual std::optional<DmaBufFrame> takeDmaBufFrame() { return std::nullopt; }
    virtual bool supportsNativeBufferTransport(NativeBufferTransport) const {
        return false;
    }
    virtual bool sendNativeBufferHandle(int, uint32_t) { return false; }
    virtual void cancelDmaBufFrame(uint32_t) {}
    virtual void releaseDmaBufFrame(uint32_t) {}
    virtual bool releaseDmaBufFrameWithFence(uint32_t bufferId, int) {
        releaseDmaBufFrame(bufferId);
        return false;
    }

    virtual void saveState() {}
    virtual void restoreState() {}
    virtual void clipRect(const RectF&) {}
    virtual void clipPath(const Path&, FillRule = FillRule::NonZero) {}
    virtual void concatTransform(const Matrix3&) {}
    virtual void beginLayer(float) {}
    virtual void endLayer() {}
    virtual void clearRect(const RectF& rect, Color color) = 0;
    virtual bool beginCachedLayer(CachedLayerId, const RectF&) { return false; }
    virtual bool beginCachedLayerUpdate(CachedLayerId, const RectF&,
                                        const RectF&) { return false; }
    virtual void endCachedLayer() {}
    virtual bool drawCachedLayer(CachedLayerId, const RectF&, float = 1.0f) { return false; }
    virtual bool drawCachedLayerTransformed(
            CachedLayerId id, const RectF& destination,
            const Matrix3& transform, float opacity = 1.0f) {
        saveState();
        concatTransform(transform);
        const bool drawn = drawCachedLayer(id, destination, opacity);
        restoreState();
        return drawn;
    }

    /** Replay backend-neutral commands through this Canvas' current state. */
    void drawDisplayList(const DisplayList& displayList);

    virtual void drawPath(const Path& path, const Paint& paint) = 0;
    virtual void drawRect(const RectF& rect, Color color) = 0;
    virtual void drawRoundedRect(const RectF& rect, float radius, Color color,
                                 Color border, float borderWidth, float roundness) = 0;
    virtual void drawTopRoundedRect(const RectF& rect, float radius, Color color,
                                    float roundness) = 0;
    virtual void drawEllipse(const RectF& rect, const Paint& paint) {
        Path path;
        path.addEllipse(rect);
        drawPath(path, paint);
    }
    virtual void drawText(float x, float y, const std::string& text, Color color,
                          float fontSize,
                          FontFamily family = FontFamily::Interface) = 0;
    virtual float measureText(const std::string& text, float fontSize,
                              FontFamily family = FontFamily::Interface) = 0;
    virtual void drawBuffer(const RectF& destination, int srcWidth, int srcHeight,
                            const uint32_t* pixels, int stridePixels, float opacity,
                            float cornerRadius, float cornerRoundness,
                            bool squareTopCorners) = 0;
    virtual void drawImageResource(const RectF& destination,
                                   const ImageResourceView& resource,
                                   float opacity, float cornerRadius,
                                   float cornerRoundness,
                                   bool squareTopCorners) {
        drawBuffer(destination, resource.width, resource.height, resource.pixels,
                   resource.stridePixels, opacity, cornerRadius,
                   cornerRoundness, squareTopCorners);
    }
    /** Internal retained-node placeholder; rasterd resolves the granted frame. */
    virtual void drawExternalBufferPlaceholder(
        uint64_t, const RectF&) {}
};

} // namespace lcl::graphics
