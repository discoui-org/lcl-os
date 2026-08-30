#pragma once

#include <cstdint>
#include <vector>

#include "platforms/common/display_backend.hpp"
#include "platforms/common/graphics_context.hpp"
#include "system/render/raster_renderer.hpp"

namespace lcl::render {

/** Owns the compositor raster backend and its physical presentation target. */
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    bool initialize(uint32_t width = 1024, uint32_t height = 768,
                    platform::IGraphicsContext* graphicsContext = nullptr,
                    platform::IDisplayBackend* displayBackend = nullptr,
                    uint32_t* fbPixels = nullptr);
    void shutdown();

    /** Frame-level operation; scene primitives are replayed by RasterRenderer. */
    void clear(uint32_t argbColor);
    void swapBuffers();

    bool isInitialized() const { return m_initialized; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint64_t getRenderedFrames() const { return m_renderedFrames; }
    RasterRenderer* getRasterRenderer() { return &m_rasterRenderer; }
    const RasterRenderer* getRasterRenderer() const { return &m_rasterRenderer; }
    platform::NativeFenceWaitResult waitNativeFence(int fenceFd);
    int createNativeFence();

private:
    platform::IGraphicsContext* m_graphicsContext{nullptr};
    platform::IDisplayBackend* m_displayBackend{nullptr};
    uint32_t* m_fbPixels{nullptr};
    RasterRenderer m_rasterRenderer;
    uint32_t m_width{1024};
    uint32_t m_height{768};
    bool m_initialized{false};
    std::vector<uint32_t> m_softwareBackBuffer;
    uint64_t m_renderedFrames{0};
};

} // namespace lcl::render
