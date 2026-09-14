#pragma once

#include "lcl-client/owned_fd.hpp"
#include "system/ipc/raster_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lcl::client {

struct LegacyImageUpload {
    uint64_t resourceId{0};
    uint64_t contentRevision{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stridePixels{0};
    bool opaque{false};
    std::span<const uint32_t> pixels{};
};

struct RasterBufferReleased {
    raster_protocol::ExternalBufferReleased message{};
    OwnedFd releaseFence{};
};

using RasterEvent = std::variant<raster_protocol::FrameDiscarded,
                                 RasterBufferReleased,
                                 raster_protocol::PresentationAnimationResult>;

/** Toolkit-neutral rasterd transport used by the legacy lcl-ui adapter. */
class RasterConnection {
public:
    RasterConnection() = default;
    ~RasterConnection();
    RasterConnection(const RasterConnection&) = delete;
    RasterConnection& operator=(const RasterConnection&) = delete;

    void configure(std::string socketPath,
                   const raster_protocol::SurfaceGrant& grant);
    void disconnect() noexcept;
    bool prepare();
    bool isConfigured() const noexcept;
    bool isConnected() const noexcept { return m_fd >= 0; }
    int fd() const noexcept { return m_fd; }
    uint64_t connectionGeneration() const noexcept { return m_generation; }
    const raster_protocol::SurfaceGrant& surfaceGrant() const noexcept {
        return m_grant;
    }

    bool uploadImage(const LegacyImageUpload& image);
    bool uploadExternalBuffer(raster_protocol::UploadExternalBuffer upload,
                              int bufferFd, int acquireFenceFd = -1);
    bool commitRetained(raster_protocol::CommitTransaction transaction,
                        std::span<const raster_protocol::NodeMutation> mutations,
                        std::span<const uint8_t> displayList);
    bool submitPresentationAnimation(
        raster_protocol::PresentationAnimation animation);
    std::vector<RasterEvent> dispatch();

private:
    bool connectIfNeeded();
    static int createSealedMemfd(const char* name, const void* data,
                                 size_t bytes);

    std::string m_socketPath;
    raster_protocol::SurfaceGrant m_grant{};
    int m_fd{-1};
    uint64_t m_generation{0};
};

} // namespace lcl::client
