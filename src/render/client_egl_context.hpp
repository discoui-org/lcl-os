#pragma once

#include <cstdint>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>

#include "core/display/egl_context.hpp"

namespace lcl::render {

// Client-only EGL target.  It opens a DRM render node, never a KMS card, so it
// cannot page-flip or otherwise own the display.  Its output is read back to
// WindowApp's existing SHM staging buffer until the DMA-BUF protocol arrives.
class ClientEGLContext final : public lcl::core::EGLContextBackend {
public:
    ClientEGLContext() = default;
    ~ClientEGLContext() override;

    ClientEGLContext(const ClientEGLContext&) = delete;
    ClientEGLContext& operator=(const ClientEGLContext&) = delete;

    bool initialize(uint32_t width, uint32_t height);
    void shutdown();

    bool isInitialized() const override { return m_initialized; }
    bool isHardwareAccelerated() const override { return m_hardwareAccelerated; }
    bool makeCurrent() override;
    bool resize(uint32_t width, uint32_t height) override;
    bool presentsToDisplay() const override { return false; }
    bool present() override { return true; }
    bool readback(uint32_t* destination, uint32_t width, uint32_t height) override;

    const std::string& rendererString() const { return m_rendererString; }

private:
    bool createSurface(uint32_t width, uint32_t height);
    static bool isSoftwareRenderer(const char* renderer);

    int m_renderFd{-1};
    gbm_device* m_gbmDevice{nullptr};
    EGLDisplay m_display{EGL_NO_DISPLAY};
    EGLContext m_context{EGL_NO_CONTEXT};
    EGLSurface m_surface{EGL_NO_SURFACE};
    EGLConfig m_config{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_surfaceless{false};
    bool m_initialized{false};
    bool m_hardwareAccelerated{false};
    std::string m_rendererString{"unavailable"};
};

} // namespace lcl::render
