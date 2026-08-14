#pragma once

#include <cstdint>

namespace lcl::core {

// Minimal EGL contract consumed by the renderer.  It deliberately contains no
// DRM/KMS ownership: compositor scanout and client offscreen contexts can both
// implement it.
class EGLContextBackend {
public:
    virtual ~EGLContextBackend() = default;

    virtual bool isInitialized() const = 0;
    virtual bool isHardwareAccelerated() const = 0;
    virtual bool makeCurrent() = 0;
    virtual bool resize(uint32_t width, uint32_t height) = 0;

    // KMS targets present; offscreen client targets read their scene FBO back
    // into their existing SHM staging buffer instead.
    virtual bool presentsToDisplay() const = 0;
    virtual bool present() = 0;
    virtual bool readback(uint32_t* destination, uint32_t width, uint32_t height) = 0;
};

} // namespace lcl::core
