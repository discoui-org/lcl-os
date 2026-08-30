#pragma once

#include "system/ipc/raster_protocol.hpp"
#include "core/retained_render_tree.hpp"
#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/widgets/external_buffer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcl::ui {

struct ExternalBufferRelease {
    raster_protocol::ExternalBufferReleased message{};
    int releaseFenceFd{-1};
};

class RasterServiceClient {
public:
    RasterServiceClient() = default;
    ~RasterServiceClient();
    RasterServiceClient(const RasterServiceClient&) = delete;
    RasterServiceClient& operator=(const RasterServiceClient&) = delete;

    void configure(std::string socketPath,
                   const raster_protocol::SurfaceGrant& grant);
    void disconnect() noexcept;
    bool prepare() { return connectIfNeeded(); }
    bool uploadImage(const graphics::ImageResourceView& resource);
    bool uploadExternalBuffer(const ExternalBufferFrame& frame);
    bool commitTransaction(
        uint64_t configureSerial, uint64_t frameSerial,
        uint64_t baseFrameSerial, uint64_t geometryGeneration,
        float logicalWidth, float logicalHeight, float bufferScale,
        const graphics::RectF& damage,
        const detail::RenderTreeTransaction& renderTreeTransaction,
        const std::vector<uint8_t>& displayList,
        uint64_t clientFrameStartNs);
    std::vector<raster_protocol::FrameDiscarded> pollDiscards();
    std::vector<ExternalBufferRelease>
        takeExternalBufferReleases();
    bool isConfigured() const noexcept;
    bool isConnected() const noexcept { return m_fd >= 0; }
    uint64_t connectionGeneration() const noexcept {
        return m_connectionGeneration;
    }

private:
    bool connectIfNeeded();
    int createSealedMemfd(const char* name, const void* data, size_t bytes);

    std::string m_socketPath;
    raster_protocol::SurfaceGrant m_grant{};
    int m_fd{-1};
    uint64_t m_connectionGeneration{0};
    std::vector<ExternalBufferRelease>
        m_externalBufferReleases;
};

} // namespace lcl::ui
