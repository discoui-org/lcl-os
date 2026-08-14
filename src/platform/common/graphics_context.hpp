#pragma once

#include <cstdint>
#include "platform/common/native_buffer.hpp"

namespace lcl::platform {

using TextureHandle = uint32_t;
constexpr TextureHandle kInvalidTextureHandle = 0;

/**
 * @brief Platform-agnostic graphics context & presentation contract.
 *
 * Consumed by Renderer and SkiaRenderer. Deliberately free of any EGL,
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

    // Offscreen / client readback
    virtual bool readback(uint32_t* destination, uint32_t width, uint32_t height) = 0;

    // Opaque polymorphic native buffer texture import/release
    virtual TextureHandle importTexture(const INativeBuffer& buffer) = 0;
    virtual void releaseTexture(TextureHandle texture) = 0;
};

} // namespace lcl::platform
