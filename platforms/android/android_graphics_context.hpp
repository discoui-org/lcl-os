#pragma once

#include "platforms/common/graphics_context.hpp"
#include "platforms/common/retained_output_damage.hpp"
#include "platforms/android/ahardware_native_buffer.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <memory>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace lcl::platform::android {

class AndroidDisplayBackend;

/**
 * @brief Android EGL/GLES Graphics Context.
 *
 * Implements IGraphicsContext on top of Android EGL and OpenGL ES 3.0.
 * Manages EGLDisplay, EGLContext, PBuffer rendering surfaces, and
 * triple-buffered AHardwareBuffer scanout targets for Android Composer presentation.
 */
class AndroidGraphicsContext final : public lcl::platform::IGraphicsContext {
public:
    AndroidGraphicsContext();
    ~AndroidGraphicsContext() override;

    // Non-copyable, non-moveable
    AndroidGraphicsContext(const AndroidGraphicsContext&) = delete;
    AndroidGraphicsContext& operator=(const AndroidGraphicsContext&) = delete;

    bool initialize(uint32_t width = 320, uint32_t height = 640,
                    AndroidDisplayBackend* displayBackend = nullptr);
    void shutdown();

    bool isInitialized() const override { return m_initialized; }
    bool isHardwareAccelerated() const override { return true; }
    bool makeCurrent() override;
    bool resize(uint32_t width, uint32_t height) override;

    bool presentsToDisplay() const override { return true; }
    bool present() override;
    bool presentFramebuffer(uint32_t framebuffer,
                            uint32_t width, uint32_t height,
                            std::optional<lcl::platform::PresentationDamage>
                                damage = std::nullopt) override;

    bool readback(uint32_t* destination, uint32_t width, uint32_t height) override;

    lcl::platform::TextureHandle importTexture(const lcl::platform::INativeBuffer& buffer) override;
    void releaseTexture(lcl::platform::TextureHandle texture) override;
    NativeFenceWaitResult waitNativeFence(int fenceFd) override;
    int createNativeFence() override;

    // EGL/GLES state accessors
    EGLDisplay getEglDisplay() const { return m_eglDisplay; }
    EGLContext getEglContext() const { return m_eglContext; }
    EGLSurface getEglSurface() const { return m_eglSurface; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    struct ScanoutSlot {
        uint32_t bufferId{0};
        AHardwareBuffer* ahb{nullptr};
        EGLImageKHR eglImage{EGL_NO_IMAGE_KHR};
        GLuint texture{0};
        GLuint fbo{0};
    };

    bool setupScanoutBuffers();
    void destroyScanoutBuffers();
    void resetScanoutDamageHistory();
    bool presentFromFramebuffer(uint32_t framebuffer,
                                uint32_t width, uint32_t height,
                                std::optional<lcl::platform::PresentationDamage>
                                    damage);

    EGLDisplay m_eglDisplay{EGL_NO_DISPLAY};
    EGLConfig m_eglConfig{nullptr};
    EGLContext m_eglContext{EGL_NO_CONTEXT};
    EGLSurface m_eglSurface{EGL_NO_SURFACE};

    AndroidDisplayBackend* m_displayBackend{nullptr};

    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_initialized{false};

    bool m_hasAhbExtension{false};
    bool m_hasImageExtension{false};

    // Three slots let GLES, Composer validation, and display scanout overlap.
    // The display backend waits on a slot's old release fences only when that
    // exact AHardwareBuffer is selected again.
    static constexpr size_t kScanoutSlotCount = 3;
    std::array<ScanoutSlot, kScanoutSlotCount> m_scanoutSlots{};
    size_t m_currentSlotIndex{0};
    lcl::platform::RetainedOutputDamageTracker m_scanoutDamage;
    uint64_t m_nextSceneSerial{1};
    uint64_t m_lastSceneSerial{0};
    uint64_t m_scanoutGeometryGeneration{0};

    // Imported textures cache
    std::unordered_map<GLuint, EGLImageKHR> m_importedImages;
};

} // namespace lcl::platform::android
