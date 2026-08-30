#include "system/compositor/raster_service_host.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace lcl::core {

namespace {

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

uint64_t randomWord() {
    uint64_t value = 0;
    const ssize_t count = getrandom(&value, sizeof(value), 0);
    if (count == static_cast<ssize_t>(sizeof(value)) && value != 0) return value;
    static uint64_t fallback = 0x9e3779b97f4a7c15ull;
    fallback ^= static_cast<uint64_t>(getpid()) +
        static_cast<uint64_t>(std::chrono::steady_clock::now()
                                  .time_since_epoch().count());
    fallback ^= fallback << 13u;
    fallback ^= fallback >> 7u;
    fallback ^= fallback << 17u;
    return fallback == 0 ? 1 : fallback;
}

} // namespace

size_t RasterServiceHost::TokenHash::operator()(const TokenKey& key) const noexcept {
    return std::hash<uint64_t>{}(key.high) ^
        (std::hash<uint64_t>{}(key.low) << 1u);
}

RasterServiceHost::TokenKey RasterServiceHost::keyOf(
        const raster_protocol::SurfaceGrant& grant) noexcept {
    return {grant.tokenHigh, grant.tokenLow};
}

RasterServiceHost::~RasterServiceHost() {
    shutdown();
}

bool RasterServiceHost::initialize(std::string executable,
                                   std::string publicSocketPath) {
    if (m_childPid > 0 || m_channelFd >= 0 || m_nativeBufferFd >= 0) return true;
    m_executable = std::move(executable);
    m_publicSocketPath = std::move(publicSocketPath);
    m_shuttingDown = false;
    return spawn();
}

bool RasterServiceHost::spawn() {
    int channels[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                   0, channels) != 0) {
        return false;
    }
    int nativeChannels[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                   0, nativeChannels) != 0) {
        close(channels[0]);
        close(channels[1]);
        return false;
    }

    constexpr int kChildChannel = 3;
    constexpr int kChildNativeChannel = 4;
    const int childChannelSource = fcntl(
        channels[1], F_DUPFD_CLOEXEC, 10);
    const int childNativeSource = fcntl(
        nativeChannels[1], F_DUPFD_CLOEXEC, 10);
    close(channels[1]);
    close(nativeChannels[1]);
    if (childChannelSource < 0 || childNativeSource < 0) {
        if (childChannelSource >= 0) close(childChannelSource);
        if (childNativeSource >= 0) close(childNativeSource);
        close(channels[0]);
        close(nativeChannels[0]);
        return false;
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(
        &actions, childChannelSource, kChildChannel);
    posix_spawn_file_actions_adddup2(
        &actions, childNativeSource, kChildNativeChannel);
    if (channels[0] != kChildChannel &&
        channels[0] != kChildNativeChannel) {
        posix_spawn_file_actions_addclose(&actions, channels[0]);
    }
    if (nativeChannels[0] != kChildChannel &&
        nativeChannels[0] != kChildNativeChannel) {
        posix_spawn_file_actions_addclose(&actions, nativeChannels[0]);
    }
    posix_spawn_file_actions_addclose(&actions, childChannelSource);
    posix_spawn_file_actions_addclose(&actions, childNativeSource);

    const std::string childFd = std::to_string(kChildChannel);
    const std::string childNativeFd = std::to_string(kChildNativeChannel);
    std::array<char*, 8> arguments{
        const_cast<char*>(m_executable.c_str()),
        const_cast<char*>("--compositor-fd"),
        const_cast<char*>(childFd.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(m_publicSocketPath.c_str()),
        const_cast<char*>("--native-buffer-fd"),
        const_cast<char*>(childNativeFd.c_str()),
        nullptr,
    };
    pid_t child = -1;
    const int status = posix_spawn(
        &child, m_executable.c_str(), &actions, nullptr,
        arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(childChannelSource);
    close(childNativeSource);
    if (status != 0) {
        close(channels[0]);
        close(nativeChannels[0]);
        std::cerr << "[LCL Raster Host] Could not start " << m_executable
                  << ": " << std::strerror(status) << "\n";
        return false;
    }

    m_channelFd = channels[0];
    m_nativeBufferFd = nativeChannels[0];
    m_childPid = child;
    m_ready = false;
    if (!setNonBlocking(m_channelFd)) {
        stopChild();
        return false;
    }
    std::cout << "[LCL Raster Host] Started raster service pid=" << child << "\n";
    return true;
}

void RasterServiceHost::stopChild() noexcept {
    if (m_channelFd >= 0) close(m_channelFd);
    m_channelFd = -1;
    if (m_nativeBufferFd >= 0) close(m_nativeBufferFd);
    m_nativeBufferFd = -1;
    clearPendingNativeLayers();
    m_ready = false;
    if (m_childPid > 0) {
        kill(m_childPid, SIGTERM);
        int status = 0;
        pid_t result = waitpid(m_childPid, &status, WNOHANG);
        if (result == 0) {
            // The supervised helper owns no user state. Ensure it cannot turn
            // into a zombie or overlap the replacement private channel.
            (void)kill(m_childPid, SIGKILL);
            do {
                result = waitpid(m_childPid, &status, 0);
            } while (result < 0 && errno == EINTR);
        }
    }
    m_childPid = -1;
}

void RasterServiceHost::clearPendingNativeLayers() noexcept {
    for (auto& pending : m_pendingNativeLayers) {
        if (pending.layer.fd >= 0) close(pending.layer.fd);
    }
    m_pendingNativeLayers.clear();
}

void RasterServiceHost::shutdown() noexcept {
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    for (auto& layer : m_readyLayers) {
        if (layer.fd >= 0) close(layer.fd);
    }
    m_readyLayers.clear();
    stopChild();
}

void RasterServiceHost::poll() {
    if (m_shuttingDown) return;
    if (m_childPid > 0) {
        int status = 0;
        const pid_t result = waitpid(m_childPid, &status, WNOHANG);
        if (result == m_childPid) {
            if (m_channelFd >= 0) close(m_channelFd);
            m_channelFd = -1;
            if (m_nativeBufferFd >= 0) close(m_nativeBufferFd);
            m_nativeBufferFd = -1;
            clearPendingNativeLayers();
            m_childPid = -1;
            m_ready = false;
            const auto delay = std::chrono::milliseconds(
                std::min(1000u, 100u << std::min(m_restartAttempt, 3u)));
            ++m_restartAttempt;
            m_nextRestart = std::chrono::steady_clock::now() + delay;
            std::cerr << "[LCL Raster Host] raster service exited; retained layers remain active\n";
        }
    }
    if (m_childPid <= 0) {
        if (std::chrono::steady_clock::now() >= m_nextRestart) (void)spawn();
        return;
    }

    while (m_channelFd >= 0) {
        raster_protocol::Header header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto status = raster_protocol::receivePacket(
            m_channelFd, header, payload, receivedFd);
        if (status == raster_protocol::ReceiveStatus::WouldBlock) break;
        if (status == raster_protocol::ReceiveStatus::Closed ||
            status == raster_protocol::ReceiveStatus::Error) {
            if (receivedFd >= 0) close(receivedFd);
            stopChild();
            m_nextRestart = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(100);
            break;
        }
        if (status != raster_protocol::ReceiveStatus::Received) {
            if (receivedFd >= 0) close(receivedFd);
            continue;
        }
        if (header.opcode == raster_protocol::Opcode::Ready && payload.empty()) {
            if (receivedFd >= 0) close(receivedFd);
            m_ready = true;
            m_restartAttempt = 0;
            sendAllGrants();
            continue;
        }
        if (const auto* ready = raster_protocol::payloadAs<
                raster_protocol::LayerReady>(
                header, payload, raster_protocol::Opcode::LayerReady)) {
            const bool authorized = m_grants.contains(keyOf(ready->grant));
            if (ready->transport ==
                    raster_protocol::LayerTransport::AndroidHardwareBuffer) {
                constexpr size_t kMaxPendingNativeLayers = 64;
                if (m_pendingNativeLayers.size() >= kMaxPendingNativeLayers) {
                    if (receivedFd >= 0) close(receivedFd);
                    stopChild();
                    m_nextRestart = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(100);
                    break;
                }
                PendingNativeLayer pending{};
                pending.layer.metadata = *ready;
                pending.layer.fd = receivedFd;
                pending.authorized = authorized;
                m_pendingNativeLayers.push_back(std::move(pending));
            } else if (receivedFd < 0 || !authorized) {
                if (receivedFd >= 0) close(receivedFd);
                continue;
            } else {
                m_readyLayers.push_back({*ready, receivedFd, {}});
            }
        } else if (receivedFd >= 0) {
            close(receivedFd);
        }
    }
    if (m_childPid > 0 && !drainNativeLayers()) {
        stopChild();
        m_nextRestart = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(100);
    }
}

bool RasterServiceHost::drainNativeLayers() {
    while (!m_pendingNativeLayers.empty()) {
        auto received = m_platformServices.receiveNativeBuffer(m_nativeBufferFd);
        if (received.status == platform::NativeBufferReceiveStatus::WouldBlock) {
            return true;
        }
        if (received.status != platform::NativeBufferReceiveStatus::Received ||
            !received.buffer) {
            std::cerr << "[LCL Raster Host] Native-buffer channel failed; "
                         "restarting raster service\n";
            return false;
        }

        PendingNativeLayer pending = std::move(m_pendingNativeLayers.front());
        m_pendingNativeLayers.pop_front();
        const auto& ready = pending.layer.metadata;
        const auto& description = received.description;
        const bool strideFits =
            description.stridePixels <= UINT32_MAX / sizeof(uint32_t);
        const bool valid = pending.authorized && ready.layerId != 0 &&
            ready.bufferId != 0 && description.width == ready.backingWidth &&
            description.height == ready.backingHeight &&
            description.layers == 1 && description.format == ready.format &&
            strideFits &&
            description.stridePixels * sizeof(uint32_t) == ready.stride;
        if (!valid) {
            std::cerr << "[LCL Raster Host] Rejected native layer "
                      << ready.layerId << " (authorized="
                      << pending.authorized << ", ready="
                      << ready.backingWidth << "x" << ready.backingHeight
                      << " stride=" << ready.stride << " format="
                      << ready.format << ", received=" << description.width
                      << "x" << description.height << " layers="
                      << description.layers << " stridePixels="
                      << description.stridePixels << " format="
                      << description.format << ")\n";
            if (pending.layer.fd >= 0) close(pending.layer.fd);
            releaseLayer(
                ready.layerId,
                pending.authorized
                    ? raster_protocol::LayerReleaseReason::RejectedFrame
                    : raster_protocol::LayerReleaseReason::SurfaceRevoked);
            continue;
        }
        pending.layer.nativeBuffer = std::move(received.buffer);
        m_readyLayers.push_back(std::move(pending.layer));
    }
    return true;
}

std::vector<RasterServiceHost::ReceivedLayer>
RasterServiceHost::takeReadyLayers() {
    return std::exchange(m_readyLayers, {});
}

raster_protocol::SurfaceGrant RasterServiceHost::registerSurface(
        uint32_t surfaceId, pid_t ownerPid, bool interactiveSystem) {
    raster_protocol::SurfaceGrant grant{};
    grant.surfaceId = surfaceId;
    grant.ownerPid = ownerPid;
    grant.flags = interactiveSystem
        ? raster_protocol::kGrantInteractiveSystem : 0;
    do {
        grant.tokenHigh = randomWord();
        grant.tokenLow = randomWord();
    } while (m_grants.contains(keyOf(grant)));
    m_grants[keyOf(grant)] = grant;
    if (m_ready) (void)sendGrant(grant);
    return grant;
}

void RasterServiceHost::revokeSurface(
        const raster_protocol::SurfaceGrant& grant) {
    m_grants.erase(keyOf(grant));
    if (m_ready) {
        (void)raster_protocol::sendPacket(
            m_channelFd, raster_protocol::Opcode::RevokeSurface, grant);
    }
}

void RasterServiceHost::releaseLayer(
        uint64_t layerId, raster_protocol::LayerReleaseReason reason,
        int releaseFenceFd) {
    if (!m_ready || layerId == 0) {
        if (releaseFenceFd >= 0) close(releaseFenceFd);
        return;
    }
    raster_protocol::ReleaseLayer release{};
    release.layerId = layerId;
    release.reason = reason;
    (void)raster_protocol::sendPacket(
        m_channelFd, raster_protocol::Opcode::ReleaseLayer, release,
        releaseFenceFd);
    if (releaseFenceFd >= 0) close(releaseFenceFd);
}

bool RasterServiceHost::sendGrant(
        const raster_protocol::SurfaceGrant& grant) {
    return m_channelFd >= 0 && raster_protocol::sendPacket(
        m_channelFd, raster_protocol::Opcode::RegisterSurface, grant);
}

void RasterServiceHost::sendAllGrants() {
    for (const auto& [_, grant] : m_grants) (void)sendGrant(grant);
}

} // namespace lcl::core
