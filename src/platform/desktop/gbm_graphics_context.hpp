#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>

#include "platform/common/graphics_context.hpp"
#include "platform/desktop/dma_buf_native_buffer.hpp"

namespace lcl::platform::desktop {

class GbmGraphicsContext final : public lcl::platform::IGraphicsContext {
public:
    GbmGraphicsContext() = default;
    ~GbmGraphicsContext() override;

    // Non-copyable
    GbmGraphicsContext(const GbmGraphicsContext&) = delete;
    GbmGraphicsContext& operator=(const GbmGraphicsContext&) = delete;

    // Moveable
    GbmGraphicsContext(GbmGraphicsContext&&) noexcept;
    GbmGraphicsContext& operator=(GbmGraphicsContext&&) noexcept;

    /**
     * @brief Initialize GBM and EGL rendering context over a DRM card file descriptor.
     * @param drmFd Open file descriptor for /dev/dri/card0
     * @param width Width of scanout surface
     * @param height Height of scanout surface
     * @param crtcId DRM CRTC ID for pageflipping
     * @param connectorId DRM Connector ID
     * @return true if EGL context successfully created, false otherwise
     */
    bool initialize(int drmFd, uint32_t width, uint32_t height, uint32_t crtcId = 0, uint32_t connectorId = 0);

    /**
     * @brief Shutdown EGL context and release GBM resources.
     */
    void shutdown();

    // IGraphicsContext implementation
    bool isInitialized() const override { return m_initialized; }
    bool isHardwareAccelerated() const override { return m_isHardwareAccelerated; }
    bool makeCurrent() override;
    bool resize(uint32_t width, uint32_t height) override;
    bool presentsToDisplay() const override { return true; }
    bool present() override { return swapBuffers(); }
    bool readback(uint32_t*, uint32_t, uint32_t) override { return false; }
    lcl::platform::TextureHandle importTexture(const lcl::platform::INativeBuffer& buffer) override;
    void releaseTexture(lcl::platform::TextureHandle texture) override;

    // Desktop/GBM specific methods
    bool swapBuffers();
    bool isVSyncActive() const { return m_vsyncActive; }
    const std::string& getGLRendererString() const { return m_glRendererString; }
    EGLDisplay getEGLDisplay() const { return m_eglDisplay; }
    EGLContext getEGLContext() const { return m_eglContext; }
    EGLSurface getEGLSurface() const { return m_eglSurface; }
    struct gbm_device* getGBMDevice() const { return m_gbmDevice; }
    struct gbm_surface* getGBMSurface() const { return m_gbmSurface; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint32_t getFBId() const { return m_currentFBId; }

private:
    uint32_t getOrCreateFB(struct gbm_bo* bo);

    int m_drmFd{-1};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_crtcId{0};
    uint32_t m_connectorId{0};

    struct gbm_device* m_gbmDevice{nullptr};
    struct gbm_surface* m_gbmSurface{nullptr};
    struct gbm_bo* m_previousBO{nullptr};

    EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
    EGLContext m_eglContext{EGL_NO_CONTEXT};
    EGLSurface m_eglSurface{EGL_NO_SURFACE};
    EGLConfig m_eglConfig{nullptr};

    uint32_t m_currentFBId{0};
    bool m_crtcSet{false};
    bool m_vsyncActive{false};
    bool m_initialized{false};
    std::string m_glRendererString{"Software Fallback"};
    bool m_isHardwareAccelerated{false};
    std::unordered_map<uint32_t, EGLImageKHR> m_importedDmaBufImages;
};

} // namespace lcl::platform::desktop
