#pragma once

#include "system/ipc/raster_protocol.hpp"
#include "core/retained_render_tree.hpp"
#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/widgets/external_buffer.hpp"
#include "lcl-client/raster_connection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcl::ui {

struct ExternalBufferRelease {
    raster_protocol::ExternalBufferReleased message{};
    int releaseFenceFd{-1};
};

/** One private raster-service event drained from the non-blocking socket. */
struct RasterServiceEvent {
    enum class Kind : uint8_t {
        FrameDiscarded,
        ExternalBufferReleased,
        PresentationAnimationResult,
    };

    Kind kind{Kind::FrameDiscarded};
    raster_protocol::FrameDiscarded discarded{};
    ExternalBufferRelease externalBufferRelease{};
    raster_protocol::PresentationAnimationResult animation{};
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
    /** Send one compositor-owned retained-presentation animation declaration. */
    bool submitPresentationAnimation(
        const raster_protocol::PresentationAnimation& animation);
    /** Drain discard, external-release, and animation-result events together. */
    std::vector<RasterServiceEvent> pollEvents();
    bool isConfigured() const noexcept;
    bool isConnected() const noexcept { return m_connection.isConnected(); }
    uint64_t connectionGeneration() const noexcept {
        return m_connection.connectionGeneration();
    }
    const raster_protocol::SurfaceGrant& surfaceGrant() const noexcept {
        return m_connection.surfaceGrant();
    }

private:
    bool connectIfNeeded();
    lcl::client::RasterConnection m_connection;
};

} // namespace lcl::ui
