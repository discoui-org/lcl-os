#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "lcl-graphics/display_list.hpp"
#include "platforms/common/graphics_context.hpp"

namespace lcl::render {

/**
 * Owns Skia/Ganesh replay below the backend-neutral LCL DisplayList contract.
 * No Skia type crosses this header or enters lcl-graphics/lcl-ui.
 */
class SkiaDisplayListRenderer final {
public:
    SkiaDisplayListRenderer();
    ~SkiaDisplayListRenderer();

    SkiaDisplayListRenderer(const SkiaDisplayListRenderer&) = delete;
    SkiaDisplayListRenderer& operator=(const SkiaDisplayListRenderer&) = delete;

    /** Pass nullptr for Skia's CPU raster backend. */
    bool initialize(lcl::platform::IGraphicsContext* graphicsContext);
    void shutdown();

    bool beginFrame(uint32_t framebuffer, uint32_t pixelWidth,
                    uint32_t pixelHeight, uint32_t* rasterPixels,
                    std::optional<lcl::graphics::RectF> deviceDamage = std::nullopt);
    bool replay(const lcl::graphics::DisplayList& displayList,
                const lcl::graphics::RenderTarget& target,
                const lcl::graphics::Matrix3& rootTransform = {},
                float deviceOriginX = 0.0f,
                float deviceOriginY = 0.0f);
    void releaseCachedLayer(uint64_t id);
    void clearCaches();
    void endFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcl::render
