#pragma once

#include "core/ipc/raster_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace lcl::core {

class RasterServiceHost {
public:
    struct ReceivedLayer {
        raster_protocol::LayerReady metadata{};
        int fd{-1};
    };

    RasterServiceHost() = default;
    ~RasterServiceHost();
    RasterServiceHost(const RasterServiceHost&) = delete;
    RasterServiceHost& operator=(const RasterServiceHost&) = delete;

    bool initialize(std::string executable, std::string publicSocketPath);
    void shutdown() noexcept;
    void poll();
    std::vector<ReceivedLayer> takeReadyLayers();

    raster_protocol::SurfaceGrant registerSurface(
        uint32_t surfaceId, pid_t ownerPid, bool interactiveSystem);
    void revokeSurface(const raster_protocol::SurfaceGrant& grant);
    void releaseLayer(
        uint64_t layerId,
        raster_protocol::LayerReleaseReason reason =
            raster_protocol::LayerReleaseReason::Presented);

    bool isReady() const noexcept { return m_ready; }

private:
    struct TokenKey {
        uint64_t high{0};
        uint64_t low{0};
        bool operator==(const TokenKey&) const = default;
    };
    struct TokenHash {
        size_t operator()(const TokenKey& key) const noexcept;
    };

    bool spawn();
    void stopChild() noexcept;
    void sendAllGrants();
    bool sendGrant(const raster_protocol::SurfaceGrant& grant);
    static TokenKey keyOf(const raster_protocol::SurfaceGrant& grant) noexcept;

    std::string m_executable;
    std::string m_publicSocketPath;
    int m_channelFd{-1};
    pid_t m_childPid{-1};
    bool m_ready{false};
    bool m_shuttingDown{false};
    unsigned m_restartAttempt{0};
    std::chrono::steady_clock::time_point m_nextRestart{};
    std::unordered_map<TokenKey, raster_protocol::SurfaceGrant, TokenHash> m_grants;
    std::vector<ReceivedLayer> m_readyLayers;
};

} // namespace lcl::core
