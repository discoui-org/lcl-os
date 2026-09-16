#pragma once

#include "lcl-client/surface_client.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace lcl::gpu {

/**
 * Non-owning description of an OpenGL ES framebuffer. The application keeps
 * ownership of the GL object and may render with an otherwise normal GLES
 * pipeline.
 */
struct GlPresentTarget {
    uint64_t bufferId{0};
    uint32_t framebuffer{0};
    uint32_t width{0};
    uint32_t height{0};
};

/**
 * Portable GLES presentation fallback. It reads a completed framebuffer into
 * immutable ARGB8888 storage and submits that storage through SurfaceClient.
 * All methods which inspect or read GL state require the application's EGL
 * context to be current on the calling thread.
 */
class GlPresentation {
public:
    virtual ~GlPresentation() = default;

    virtual bool prepare(std::string& error) = 0;
    virtual bool createTarget(uint32_t framebuffer, uint32_t width,
                              uint32_t height, GlPresentTarget& target,
                              std::string& error) = 0;
    virtual bool submitFrame(lcl::client::SurfaceClient& surface,
                             const GlPresentTarget& target,
                             uint64_t contentRevision,
                             const lcl::client::Rect& damage,
                             bool opaque,
                             std::string& error) = 0;
    virtual void destroyTarget(GlPresentTarget& target) noexcept = 0;
};

std::unique_ptr<GlPresentation> createGlPresentation();

} // namespace lcl::gpu
