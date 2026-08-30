#pragma once

#include <cstdint>
#include <optional>
#include "platform/common/native_buffer.hpp"

namespace lcl::platform {

enum class NativeFenceWaitResult {
    Unsupported,
    Enqueued,
    ConsumedFailure,
};

using TextureHandle = uint32_t;
constexpr TextureHandle kInvalidTextureHandle = 0;

/**
 * Changed region in the pixel coordinate space of a framebuffer presented to
 * the platform.  Coordinates use a top-left origin so every producer can
 * carry the same damage contract across its final presentation boundary.
 */
struct PresentationDamage {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    bool isEmpty() const noexcept {
        return width <= 0.0f || height <= 0.0f;
    }
};

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
    virtual bool presentFramebuffer(
            uint32_t framebuffer, uint32_t width, uint32_t height,
            std::optional<PresentationDamage> damage = std::nullopt) {
        (void)framebuffer;
        (void)width;
        (void)height;
        (void)damage;
        return false;
    }

    // Offscreen / client readback
    virtual bool readback(uint32_t* destination, uint32_t width, uint32_t height) = 0;

    // Opaque polymorphic native buffer texture import/release
    virtual TextureHandle importTexture(const INativeBuffer& buffer) = 0;
    virtual void releaseTexture(TextureHandle texture) = 0;

    // Optional explicit-sync hooks for native buffers. This must enqueue a
    // server-side dependency rather than block the compositor thread.
    // Enqueued and ConsumedFailure consume the supplied sync-file fd; a
    // returned fence fd is owned by the caller. Backends without explicit
    // sync return Unsupported immediately.
    virtual NativeFenceWaitResult waitNativeFence(int) {
        return NativeFenceWaitResult::Unsupported;
    }
    virtual int createNativeFence() { return -1; }

    // IPC DMA-BUF descriptor convenience import
    virtual TextureHandle importDmaBuf(const DmaBufDescriptor& descriptor) {
        (void)descriptor;
        return kInvalidTextureHandle;
    }
};

} // namespace lcl::platform
