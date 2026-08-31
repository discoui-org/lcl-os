#include "system/security/security_admin_client.hpp"

#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

constexpr int kSecurityAdminReplyTimeoutMs = 5000;

} // namespace

SecurityAdminClient::~SecurityAdminClient() { close(); }

bool SecurityAdminClient::connectAsSessionAuthority(const std::string& socketPath,
                                                     std::string& error) {
    error.clear();
    if (descriptor_ >= 0) {
        return true;
    }
    if (geteuid() != 0) {
        error = "only root sessiond may open lcl-securityd";
        return false;
    }
    if (socketPath.empty() || socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "securityd socket path is invalid";
        return false;
    }
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        error = std::strerror(errno);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::strerror(errno);
        ::close(descriptor);
        return false;
    }
    descriptor_ = descriptor;
    return true;
}

bool SecurityAdminClient::adoptCapabilityDescriptor(int descriptor, std::string& error) {
    error.clear();
    if (descriptor_ >= 0 || descriptor < 0) {
        error = "security capability descriptor is unavailable";
        return false;
    }
    int socketType = 0;
    socklen_t socketTypeSize = sizeof(socketType);
    if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socketType, &socketTypeSize) != 0 ||
        socketTypeSize != sizeof(socketType) || socketType != SOCK_SEQPACKET) {
        error = "security capability descriptor is not a sequenced packet socket";
        return false;
    }
    const int flags = fcntl(descriptor, F_GETFD);
    if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
        error = std::strerror(errno);
        return false;
    }
    descriptor_ = descriptor;
    return true;
}

int SecurityAdminClient::releaseCapabilityDescriptor() noexcept {
    return std::exchange(descriptor_, -1);
}

void SecurityAdminClient::close() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

bool SecurityAdminClient::sendPacket(SecurityAdminOpcode opcode,
                                     const std::vector<std::uint8_t>& payload,
                                     std::uint32_t& requestId, std::string& error) {
    error.clear();
    if (descriptor_ < 0) {
        error = "not connected to lcl-securityd";
        return false;
    }
    requestId = nextRequestId_++;
    if (nextRequestId_ == 0) {
        nextRequestId_ = 1;
    }
    SecurityAdminHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    if (!encodeSecurityAdminPacket(header, payload, packet) ||
        send(descriptor_, packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size())) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool SecurityAdminClient::receivePacket(DecodedSecurityAdminPacket& packet, std::string& error) {
    error.clear();
    std::array<std::uint8_t, kSecurityAdminWireHeaderSize + kSecurityAdminMaxPayload> bytes{};
    const ssize_t count = recv(descriptor_, bytes.data(), bytes.size(), 0);
    if (count <= 0 || !decodeSecurityAdminPacket(bytes.data(), static_cast<std::size_t>(count), packet)) {
        error = count == 0 ? "lcl-securityd closed the connection"
                           : (count < 0 ? std::strerror(errno) : "invalid securityd packet");
        return false;
    }
    if (packet.header.opcode == SecurityAdminOpcode::ErrorResponse) {
        if (!decodeSecurityAdminError(packet.payload, error)) {
            error = "invalid lcl-securityd error response";
        }
        return false;
    }
    return true;
}

bool SecurityAdminClient::pendingBundleApprovals(std::vector<PendingBundleApproval>& pending,
                                                 std::string& error) {
    std::uint32_t requestId = 0;
    if (!sendPacket(SecurityAdminOpcode::PendingBundleApprovalsRequest, {}, requestId, error)) {
        return false;
    }
    pollfd descriptor{.fd = descriptor_, .events = POLLIN, .revents = 0};
    const int ready = poll(&descriptor, 1, kSecurityAdminReplyTimeoutMs);
    if (ready <= 0) {
        error = ready == 0 ? "timed out waiting for lcl-securityd" : std::strerror(errno);
        return false;
    }
    DecodedSecurityAdminPacket packet{};
    if (!receivePacket(packet, error)) {
        return false;
    }
    if (packet.header.requestId != requestId ||
        packet.header.opcode != SecurityAdminOpcode::PendingBundleApprovalsResponse ||
        !decodePendingBundleApprovals(packet.payload, pending)) {
        error = "invalid lcl-securityd pending bundle approvals response";
        return false;
    }
    return true;
}

bool SecurityAdminClient::approveBundle(const SecurityAdminBundleIdentity& identity,
                                        std::string& error) {
    return changeApproval(SecurityAdminOpcode::ApproveBundleRequest,
                          SecurityAdminOpcode::ApproveBundleResponse, identity, error);
}

bool SecurityAdminClient::revokeBundleApproval(const SecurityAdminBundleIdentity& identity,
                                                std::string& error) {
    return changeApproval(SecurityAdminOpcode::RevokeBundleApprovalRequest,
                          SecurityAdminOpcode::RevokeBundleApprovalResponse, identity, error);
}

bool SecurityAdminClient::changeApproval(SecurityAdminOpcode requestOpcode,
                                         SecurityAdminOpcode responseOpcode,
                                         const SecurityAdminBundleIdentity& identity,
                                         std::string& error) {
    std::vector<std::uint8_t> payload;
    if (!encodeSecurityAdminBundleIdentity(identity, payload)) {
        error = "bundle approval identity is invalid";
        return false;
    }
    std::uint32_t requestId = 0;
    if (!sendPacket(requestOpcode, payload, requestId, error)) {
        return false;
    }
    pollfd descriptor{.fd = descriptor_, .events = POLLIN, .revents = 0};
    const int ready = poll(&descriptor, 1, kSecurityAdminReplyTimeoutMs);
    if (ready <= 0) {
        error = ready == 0 ? "timed out waiting for lcl-securityd" : std::strerror(errno);
        return false;
    }
    DecodedSecurityAdminPacket packet{};
    SecurityAdminApprovalResult result{};
    if (!receivePacket(packet, error)) {
        return false;
    }
    if (packet.header.requestId != requestId || packet.header.opcode != responseOpcode ||
        !decodeSecurityAdminApprovalResult(packet.payload, result)) {
        error = "invalid lcl-securityd approval response";
        return false;
    }
    if (!result.accepted) {
        error = result.message;
        return false;
    }
    return true;
}

} // namespace lcl::security
