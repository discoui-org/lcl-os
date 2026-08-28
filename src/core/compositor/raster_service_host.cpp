#include "core/compositor/raster_service_host.hpp"

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
    if (m_childPid > 0 || m_channelFd >= 0) return true;
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

    constexpr int kChildChannel = 3;
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, channels[1], kChildChannel);
    posix_spawn_file_actions_addclose(&actions, channels[0]);
    if (channels[1] != kChildChannel) {
        posix_spawn_file_actions_addclose(&actions, channels[1]);
    }

    const std::string childFd = std::to_string(kChildChannel);
    std::array<char*, 6> arguments{
        const_cast<char*>(m_executable.c_str()),
        const_cast<char*>("--compositor-fd"),
        const_cast<char*>(childFd.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(m_publicSocketPath.c_str()),
        nullptr,
    };
    pid_t child = -1;
    const int status = posix_spawn(
        &child, m_executable.c_str(), &actions, nullptr,
        arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(channels[1]);
    if (status != 0) {
        close(channels[0]);
        std::cerr << "[LCL Raster Host] Could not start " << m_executable
                  << ": " << std::strerror(status) << "\n";
        return false;
    }

    m_channelFd = channels[0];
    setNonBlocking(m_channelFd);
    m_childPid = child;
    m_ready = false;
    std::cout << "[LCL Raster Host] Started raster service pid=" << child << "\n";
    return true;
}

void RasterServiceHost::stopChild() noexcept {
    if (m_channelFd >= 0) close(m_channelFd);
    m_channelFd = -1;
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
            if (receivedFd < 0 || !m_grants.contains(keyOf(ready->grant))) {
                if (receivedFd >= 0) close(receivedFd);
                continue;
            }
            m_readyLayers.push_back({*ready, receivedFd});
        } else if (receivedFd >= 0) {
            close(receivedFd);
        }
    }
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
        uint64_t layerId, raster_protocol::LayerReleaseReason reason) {
    if (!m_ready || layerId == 0) return;
    raster_protocol::ReleaseLayer release{};
    release.layerId = layerId;
    release.reason = reason;
    (void)raster_protocol::sendPacket(
        m_channelFd, raster_protocol::Opcode::ReleaseLayer, release);
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
