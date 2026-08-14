#pragma once

#include <cstdint>

namespace lcl::core {

struct DmaBufImport {
    int fd{-1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
};

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

    // Only compositor EGL targets import client allocations. Offscreen client
    // targets retain the default no-op implementation.
    virtual uint32_t importDmaBufTexture(const DmaBufImport&) { return 0; }
    virtual void releaseDmaBufTexture(uint32_t) {}
};

} // namespace lcl::core
