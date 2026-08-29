#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#if !defined(__ANDROID__)
#include <gbm.h>
#else
struct AHardwareBuffer;
struct gbm_bo;
struct gbm_device;
#endif

#include "platform/common/graphics_context.hpp"

namespace lcl::render {

// Client-only EGL target.  It opens a DRM render node, never a KMS card, so it
// cannot page-flip or otherwise own the display.  Its output is read back to
// Optional local GPU render target. Its export metadata is consumed only by
// trusted raster backends; v27 applications submit sealed DisplayLists.
class ClientEGLContext final : public lcl::platform::IGraphicsContext {
public:
    struct DmaBufTarget {
        uint32_t bufferId{0};
        uint32_t framebuffer{0};
        uint32_t texture{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    struct DmaBufExport {
        uint32_t bufferId{0};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        uint32_t format{0};
        uint64_t modifier{~uint64_t{0}};
        int fd{-1};
        bool androidHardwareBuffer{false};
        int acquireFenceFd{-1};
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
    lcl::platform::TextureHandle importTexture(
        const lcl::platform::INativeBuffer&) override {
        return lcl::platform::kInvalidTextureHandle;
    }
    lcl::platform::TextureHandle importDmaBuf(
        const lcl::platform::DmaBufDescriptor& descriptor) override;
    void releaseTexture(lcl::platform::TextureHandle texture) override;
    lcl::platform::NativeFenceWaitResult waitNativeFence(
        int fenceFd) override;
    int createNativeFence() override;

    bool hasDmaBufPool() const { return !m_dmaBufs.empty(); }
    bool usesAndroidHardwareBuffer() const {
#if defined(__ANDROID__)
        return hasDmaBufPool();
#else
        return false;
#endif
    }
    bool ensureDmaBufCapacity(uint32_t width, uint32_t height);
    std::optional<DmaBufTarget> acquireDmaBufTarget();
    std::optional<DmaBufExport> exportCurrentDmaBuf();
    bool sendNativeBufferHandle(int socketFd, uint32_t bufferId);
    void cancelCurrentDmaBuf();
    void releaseDmaBuf(uint32_t bufferId, int releaseFenceFd = -1);

    const std::string& rendererString() const { return m_rendererString; }

private:
    struct DmaBufSlot;
    bool createSurface(uint32_t width, uint32_t height);
    bool createDmaBufPool(uint32_t width, uint32_t height);
    bool appendDmaBufPool(uint32_t width, uint32_t height);
    void destroyDmaBufSlot(DmaBufSlot& slot);
    void destroyDmaBufPool();
    static bool isSoftwareRenderer(const char* renderer);

    struct DmaBufSlot {
        uint32_t id{0};
        gbm_bo* bo{nullptr};
#if defined(__ANDROID__)
        AHardwareBuffer* ahb{nullptr};
#endif
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        uint32_t texture{0};
        uint32_t framebuffer{0};
        uint32_t stride{0};
        uint64_t modifier{~uint64_t{0}};
        uint32_t width{0};
        uint32_t height{0};
        bool busy{false};
        bool retired{false};
        int releaseFenceFd{-1};
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
    uint32_t m_nextDmaBufId{1};
    uint32_t m_dmaBufCapacityWidth{0};
    uint32_t m_dmaBufCapacityHeight{0};
    bool m_dmaBufTransportLogged{false};
    std::unordered_map<uint32_t, EGLImageKHR> m_importedDmaBufImages;
};

} // namespace lcl::render
