#include "render/renderer.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace lcl::render {

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(uint32_t width, uint32_t height,
                          platform::IGraphicsContext* graphicsContext,
                          platform::IDisplayBackend* displayBackend,
                          uint32_t* fbPixels) {
    if (m_initialized) return true;

    m_width = width > 0 ? width : 1024;
    m_height = height > 0 ? height : 768;
    m_graphicsContext = graphicsContext;
    m_displayBackend = displayBackend;
    m_fbPixels = fbPixels;
    m_softwareBackBuffer.assign(
        static_cast<size_t>(m_width) * m_height, 0xFF000000u);

    const bool initialized = m_rasterRenderer.initialize(
        m_width, m_height, m_graphicsContext, m_softwareBackBuffer.data());
    m_initialized = true;
    std::cout << "[LCL Render] Raster renderer initialized ("
              << m_width << "x" << m_height << ").\n";
    return initialized;
}

void Renderer::clear(uint32_t argbColor) {
    if (m_rasterRenderer.getBackendType() == RasterBackend::OpenGL_EGL) {
        // beginFrame clears the GPU scene and keeps the software overlay transparent.
        return;
    }
    std::fill(m_softwareBackBuffer.begin(), m_softwareBackBuffer.end(), argbColor);
}

void Renderer::swapBuffers() {
    ++m_renderedFrames;
    if (m_rasterRenderer.getBackendType() == RasterBackend::OpenGL_EGL) {
        m_rasterRenderer.endFrame();
        return;
    }

    const uint32_t* pixels = m_rasterRenderer.getRasterBuffer();
    if (!pixels) pixels = m_softwareBackBuffer.data();
    if (m_fbPixels && pixels) {
        std::memcpy(m_fbPixels, pixels,
                    static_cast<size_t>(m_width) * m_height * sizeof(uint32_t));
    }
}

platform::NativeFenceWaitResult Renderer::waitNativeFence(int fenceFd) {
    return m_graphicsContext
        ? m_graphicsContext->waitNativeFence(fenceFd)
        : platform::NativeFenceWaitResult::Unsupported;
}

int Renderer::createNativeFence() {
    return m_graphicsContext ? m_graphicsContext->createNativeFence() : -1;
}

void Renderer::shutdown() {
    if (!m_initialized) return;
    m_rasterRenderer.shutdown();
    m_initialized = false;
    std::cout << "[LCL Render] Renderer stopped after "
              << m_renderedFrames << " frames.\n";
}

} // namespace lcl::render
