#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
    struct DmaBufTarget {
        uint32_t bufferId{0};
        uint32_t framebuffer{0};
        uint32_t texture{0};
    };

    struct DmaBufExport {
        uint32_t bufferId{0};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        uint32_t format{0};
        uint64_t modifier{~uint64_t{0}};
        int fd{-1};
    };
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

    bool hasDmaBufPool() const { return !m_dmaBufs.empty(); }
    std::optional<DmaBufTarget> acquireDmaBufTarget();
    std::optional<DmaBufExport> exportCurrentDmaBuf();
    void cancelCurrentDmaBuf();
    void releaseDmaBuf(uint32_t bufferId);

    const std::string& rendererString() const { return m_rendererString; }

private:
    bool createSurface(uint32_t width, uint32_t height);
    bool createDmaBufPool(uint32_t width, uint32_t height);
    void destroyDmaBufPool();
    static bool isSoftwareRenderer(const char* renderer);

    struct DmaBufSlot {
        uint32_t id{0};
        gbm_bo* bo{nullptr};
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        uint32_t texture{0};
        uint32_t framebuffer{0};
        uint32_t stride{0};
        uint64_t modifier{~uint64_t{0}};
        bool busy{false};
    };

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
    std::vector<DmaBufSlot> m_dmaBufs;
    int m_currentDmaBuf{-1};
    bool m_dmaBufTransportLogged{false};
};

} // namespace lcl::render
