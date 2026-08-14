#pragma once

#include <cstdint>
#include <string>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "core/display/egl_context.hpp"

namespace lcl::core {

struct EGLBuffer {
    struct gbm_bo* bo{nullptr};
    uint32_t fbId{0};
    uint32_t handle{0};
    uint32_t pitch{0};
};

class EGLBackend : public EGLContextBackend {
public:
    EGLBackend() = default;
    ~EGLBackend();

    // Non-copyable
    EGLBackend(const EGLBackend&) = delete;
    EGLBackend& operator=(const EGLBackend&) = delete;

    // Moveable
    EGLBackend(EGLBackend&&) noexcept;
    EGLBackend& operator=(EGLBackend&&) noexcept;

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

    /**
     * @brief Make EGL context current on calling thread.
     */
    bool makeCurrent() override;

    /**
     * @brief Swap EGL buffers and flip DRM page.
     * @return true on successful frame presentation
     */
    bool swapBuffers();
    bool resize(uint32_t width, uint32_t height) override;
    bool presentsToDisplay() const override { return true; }
    bool present() override { return swapBuffers(); }
    bool readback(uint32_t*, uint32_t, uint32_t) override { return false; }

    bool isInitialized() const override { return m_initialized; }
    bool isVSyncActive() const { return m_vsyncActive; }
    const std::string& getGLRendererString() const { return m_glRendererString; }
    bool isHardwareAccelerated() const override { return m_isHardwareAccelerated; }
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
};

} // namespace lcl::core
