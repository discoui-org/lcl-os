#pragma once

#include "platform/common/graphics_context.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <memory>
#include <string>

namespace lcl::platform::android {

/**
 * @brief Android EGL/GLES Graphics Context.
 *
 * Implements IGraphicsContext on top of Android EGL and OpenGL ES 3.0.
 * Manages EGLDisplay, EGLContext, and offscreen/presentation surfaces.
 */
class AndroidGraphicsContext final : public lcl::platform::IGraphicsContext {
public:
    AndroidGraphicsContext();
    ~AndroidGraphicsContext() override;

    // Non-copyable, non-moveable
    AndroidGraphicsContext(const AndroidGraphicsContext&) = delete;
    AndroidGraphicsContext& operator=(const AndroidGraphicsContext&) = delete;

    bool initialize(uint32_t width = 320, uint32_t height = 640);
    void shutdown();

    bool isInitialized() const override { return m_initialized; }
    bool isHardwareAccelerated() const override { return true; }
    bool makeCurrent() override;
    bool resize(uint32_t width, uint32_t height) override;

    bool presentsToDisplay() const override { return true; }
    bool present() override;

    bool readback(uint32_t* destination, uint32_t width, uint32_t height) override;

    lcl::platform::TextureHandle importTexture(const lcl::platform::INativeBuffer& buffer) override;
    void releaseTexture(lcl::platform::TextureHandle texture) override;

    // EGL/GLES state accessors
    EGLDisplay getEglDisplay() const { return m_eglDisplay; }
    EGLContext getEglContext() const { return m_eglContext; }
    EGLSurface getEglSurface() const { return m_eglSurface; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
    EGLConfig m_eglConfig{nullptr};
    EGLContext m_eglContext{EGL_NO_CONTEXT};
    EGLSurface m_eglSurface{EGL_NO_SURFACE};

    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_initialized{false};

    bool m_hasAhbExtension{false};
    bool m_hasImageExtension{false};
};

} // namespace lcl::platform::android
