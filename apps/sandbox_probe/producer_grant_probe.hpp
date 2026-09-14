#pragma once

#include "system/ipc/raster_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace lcl::sandbox_probe {

class ProbeFd final {
public:
    explicit ProbeFd(int value = -1) : m_value(value) {}
    ~ProbeFd() { if (m_value >= 0) close(m_value); }
    ProbeFd(const ProbeFd&) = delete;
    ProbeFd& operator=(const ProbeFd&) = delete;
    int get() const { return m_value; }

private:
    int m_value{-1};
};

inline int connectRaster(const std::string& path) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline bool checkGrant(
        int fd, const raster_protocol::SurfaceGrant& grant,
        raster_protocol::DiscardReason expected, bool expectClosed = false) {
    raster_protocol::CommitTransaction transaction{};
    transaction.grant = grant;
    transaction.frameSerial = 0x4752414e54;
    // Deliberately invalid frame metadata: authorization is evaluated first,
    // so the response distinguishes a valid grant from a forged one.
    if (!raster_protocol::sendCommitTransaction(fd, transaction, {}, -1)) {
        return false;
    }
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, 2000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) return false;
    raster_protocol::Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    const auto status = raster_protocol::receivePacket(
        fd, header, payload, receivedFd);
    ProbeFd received(receivedFd);
    if (expectClosed) {
        return status == raster_protocol::ReceiveStatus::Closed;
    }
    const auto* discarded =
        raster_protocol::payloadAs<raster_protocol::FrameDiscarded>(
            header, payload, raster_protocol::Opcode::FrameDiscarded);
    return status == raster_protocol::ReceiveStatus::Received && discarded &&
        discarded->surfaceId == grant.surfaceId &&
        discarded->frameSerial == transaction.frameSerial &&
        discarded->reason == expected;
}

enum class GrantAttack {
    NewConnection,
    RewrittenOwner,
    InheritedConnection,
};

inline bool rejectsChildGrant(
        const std::string& path, raster_protocol::SurfaceGrant grant,
        GrantAttack attack) {
    ProbeFd inherited(attack == GrantAttack::InheritedConnection
        ? connectRaster(path) : -1);
    if (attack == GrantAttack::InheritedConnection &&
        !checkGrant(inherited.get(), grant,
                    raster_protocol::DiscardReason::InvalidFrame)) {
        return false;
    }
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        const bool transfer = attack == GrantAttack::InheritedConnection;
        const int fd = transfer ? inherited.get() : connectRaster(path);
        if (attack == GrantAttack::RewrittenOwner) {
            grant.ownerPid = static_cast<int32_t>(getpid());
        }
        const bool rejected = fd >= 0 && checkGrant(
            fd, grant, raster_protocol::DiscardReason::InvalidGrant, transfer);
        _exit(rejected ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
}

} // namespace lcl::sandbox_probe
