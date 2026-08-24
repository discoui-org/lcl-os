#pragma once

#include <cstdint>
#include "platform/common/native_buffer.hpp"

namespace lcl::platform {

using TextureHandle = uint32_t;
constexpr TextureHandle kInvalidTextureHandle = 0;

/**
 * @brief Platform-agnostic graphics context & presentation contract.
 *
 * Consumed by Renderer and RasterRenderer. Deliberately free of any EGL,
 * GBM, DRM, or Android types.
 */
class IGraphicsContext {
public:
    virtual ~IGraphicsContext() = default;

    virtual bool isInitialized() const = 0;
    virtual bool isHardwareAccelerated() const = 0;
    virtual bool makeCurrent() = 0;
    virtual bool resize(uint32_t width, uint32_t height) = 0;

    // Direct display presentation (Compositor context)
    virtual bool presentsToDisplay() const = 0;
    virtual bool present() = 0;
    /**
     * Present an already rendered GPU framebuffer without first copying it to
     * the context's default surface. Backends that cannot import/blit an FBO
     * keep the default false result and use present() instead.
     */
    virtual bool presentFramebuffer(uint32_t framebuffer,
                                    uint32_t width, uint32_t height) {
        (void)framebuffer;
        (void)width;
        (void)height;
        return false;
    }

    // Offscreen / client readback
    virtual bool readback(uint32_t* destination, uint32_t width, uint32_t height) = 0;

    // Opaque polymorphic native buffer texture import/release
    virtual TextureHandle importTexture(const INativeBuffer& buffer) = 0;
    virtual void releaseTexture(TextureHandle texture) = 0;

    // Optional explicit-sync hooks for native buffers. A successful wait
    // consumes the supplied sync-file fd. A returned fence fd is owned by the
    // caller. Backends without explicit sync retain their existing contract.
    virtual bool waitNativeFence(int) { return false; }
    virtual int createNativeFence() { return -1; }

    // IPC DMA-BUF descriptor convenience import
    virtual TextureHandle importDmaBuf(const DmaBufDescriptor& descriptor) {
        (void)descriptor;
        return kInvalidTextureHandle;
    }
};

} // namespace lcl::platform
