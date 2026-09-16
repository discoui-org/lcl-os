#include "lcl-gpu/gl_presentation.hpp"

#include "platforms/common/native_buffer.hpp"

#include <GLES2/gl2.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace lcl::gpu {
namespace {

constexpr uint32_t kMaxDimension = 16384;
constexpr uint64_t kMaxFrameBytes = 512ull * 1024ull * 1024ull;

std::atomic<uint64_t> g_nextBufferId{1};

int createSealedFrame(const uint32_t* pixels, size_t bytes,
                      std::string& error) {
    if (!pixels || bytes == 0) {
        error = "cannot create an empty GL presentation frame";
        return -1;
    }
#if defined(SYS_memfd_create)
    const int rawFd = static_cast<int>(syscall(
        SYS_memfd_create, "lcl-gl-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    lcl::client::OwnedFd fd(rawFd);
    if (!fd || ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0) {
        error = std::string{"could not allocate GL presentation storage: "} +
            std::strerror(errno);
        return -1;
    }
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd.get(), 0);
    if (mapping == MAP_FAILED) {
        error = std::string{"could not map GL presentation storage: "} +
            std::strerror(errno);
        return -1;
    }
    std::memcpy(mapping, pixels, bytes);
    munmap(mapping, bytes);
#if defined(F_ADD_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (fcntl(fd.get(), F_ADD_SEALS, seals) != 0) {
        error = std::string{"could not seal GL presentation storage: "} +
            std::strerror(errno);
        return -1;
    }
#endif
    return fd.release();
#else
    (void)pixels;
    (void)bytes;
    error = "immutable shared-memory frames are unavailable on this platform";
    return -1;
#endif
}

class ReadbackGlPresentation final : public GlPresentation {
public:
    bool prepare(std::string& error) override {
        if (!glGetString(GL_VERSION)) {
            error = "no current OpenGL ES context";
            return false;
        }
        error.clear();
        return true;
    }

    bool createTarget(uint32_t framebuffer, uint32_t width,
                      uint32_t height, GlPresentTarget& target,
                      std::string& error) override {
        if (width == 0 || height == 0 || width > kMaxDimension ||
            height > kMaxDimension) {
            error = "invalid GL presentation target extent";
            return false;
        }
        const uint64_t bytes = static_cast<uint64_t>(width) * height *
            sizeof(uint32_t);
        if (bytes > kMaxFrameBytes) {
            error = "GL presentation target exceeds the frame-size limit";
            return false;
        }
        uint64_t id = g_nextBufferId.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            id = g_nextBufferId.fetch_add(1, std::memory_order_relaxed);
        }
        target = GlPresentTarget{id, framebuffer, width, height};
        error.clear();
        return true;
    }

    bool submitFrame(lcl::client::SurfaceClient& surface,
                     const GlPresentTarget& target,
                     uint64_t contentRevision,
                     const lcl::client::Rect& damage,
                     bool opaque,
                     std::string& error) override {
        if (target.bufferId == 0 || target.width == 0 || target.height == 0 ||
            contentRevision == 0) {
            error = "invalid GL presentation frame identity";
            return false;
        }
        if (!prepare(error)) return false;

        GLint previousFramebuffer = 0;
        GLint previousPackAlignment = 4;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER,
                              static_cast<GLuint>(previousFramebuffer));
            error = "GL presentation framebuffer is incomplete";
            return false;
        }

        const size_t pixelCount = static_cast<size_t>(target.width) *
            target.height;
        std::vector<uint8_t> rgba(pixelCount * sizeof(uint32_t));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(target.width),
                     static_cast<GLsizei>(target.height), GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
        const GLenum readError = glGetError();
        glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
        glBindFramebuffer(GL_FRAMEBUFFER,
                          static_cast<GLuint>(previousFramebuffer));
        if (readError != GL_NO_ERROR) {
            error = "glReadPixels failed with error " +
                std::to_string(static_cast<uint32_t>(readError));
            return false;
        }

        // GLES readback is bottom-up RGBA. LCL's ARGB8888 wire format is a
        // top-down 0xAARRGGBB word stream (BGRA bytes on little-endian CPUs).
        std::vector<uint32_t> argb(pixelCount);
        for (uint32_t y = 0; y < target.height; ++y) {
            const uint32_t sourceY = target.height - 1 - y;
            for (uint32_t x = 0; x < target.width; ++x) {
                const size_t source =
                    (static_cast<size_t>(sourceY) * target.width + x) * 4;
                const uint8_t red = rgba[source];
                const uint8_t green = rgba[source + 1];
                const uint8_t blue = rgba[source + 2];
                const uint8_t alpha = rgba[source + 3];
                argb[static_cast<size_t>(y) * target.width + x] =
                    (static_cast<uint32_t>(alpha) << 24u) |
                    (static_cast<uint32_t>(red) << 16u) |
                    (static_cast<uint32_t>(green) << 8u) | blue;
            }
        }

        const size_t bytes = argb.size() * sizeof(uint32_t);
        lcl::client::OwnedFd storage(
            createSealedFrame(argb.data(), bytes, error));
        if (!storage) return false;

        lcl::client::SharedMemoryFrame frame{};
        frame.bufferId = target.bufferId;
        frame.contentRevision = contentRevision;
        frame.width = target.width;
        frame.height = target.height;
        frame.stride = target.width * sizeof(uint32_t);
        frame.format = lcl::platform::kDmaBufFormatArgb8888;
        frame.damage = damage;
        frame.opaque = opaque;
        frame.buffer = std::move(storage);
        if (!surface.submitFrame(std::move(frame))) {
            error = "could not submit GL readback frame to SurfaceClient";
            return false;
        }
        error.clear();
        return true;
    }

    void destroyTarget(GlPresentTarget& target) noexcept override {
        target = {};
    }
};

} // namespace

std::unique_ptr<GlPresentation> createGlPresentation() {
    return std::make_unique<ReadbackGlPresentation>();
}

} // namespace lcl::gpu
