#include "system/security/sandbox_client.hpp"

#include <poll.h>
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

constexpr int kLaunchReplyTimeoutMs = 5000;

} // namespace

SandboxClient::~SandboxClient() { close(); }

bool SandboxClient::connect(const std::string& socketPath, std::string& error) {
    error.clear();
    if (descriptor_ >= 0) {
        return true;
    }
    if (socketPath.empty() || socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "sandboxd socket path is invalid";
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

void SandboxClient::close() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    pendingExits_.clear();
}

bool SandboxClient::sendPacket(SandboxOpcode opcode, const std::vector<std::uint8_t>& payload,
                                std::uint32_t& requestId, std::string& error) {
    error.clear();
    if (descriptor_ < 0) {
        error = "not connected to lcl-sandboxd";
        return false;
    }
    requestId = nextRequestId_++;
    if (nextRequestId_ == 0) {
        nextRequestId_ = 1;
    }
    SandboxHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    if (!encodeSandboxPacket(header, payload, packet) ||
        send(descriptor_, packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size())) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool SandboxClient::receivePacket(DecodedSandboxPacket& packet, int flags, std::string& error) {
    error.clear();
    std::array<std::uint8_t, kSandboxWireHeaderSize + kSandboxMaxPayload> bytes{};
    const ssize_t count = recv(descriptor_, bytes.data(), bytes.size(), flags);
    if (count < 0) {
        if ((flags & MSG_DONTWAIT) != 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        error = std::strerror(errno);
        return false;
    }
    if (count == 0 || !decodeSandboxPacket(bytes.data(), static_cast<std::size_t>(count), packet)) {
        error = count == 0 ? "lcl-sandboxd closed the connection" : "invalid sandboxd packet";
        return false;
    }
    if (packet.header.opcode == SandboxOpcode::ErrorResponse) {
        if (!decodeSandboxError(packet.payload, error)) {
            error = "invalid sandboxd error response";
        }
        return false;
    }
    return true;
}

bool SandboxClient::launch(const SandboxLaunchRequest& request, SandboxLaunchResult& result,
                           std::string& error) {
    error.clear();
    std::vector<std::uint8_t> payload;
    if (!encodeSandboxLaunchRequest(request, payload)) {
        error = "sandbox launch request is invalid";
        return false;
    }
    std::uint32_t requestId = 0;
    if (!sendPacket(SandboxOpcode::LaunchRequest, payload, requestId, error)) {
        return false;
    }
    while (true) {
        pollfd waitDescriptor{.fd = descriptor_, .events = POLLIN, .revents = 0};
        const int ready = poll(&waitDescriptor, 1, kLaunchReplyTimeoutMs);
        if (ready <= 0) {
            error = ready == 0 ? "timed out waiting for lcl-sandboxd" : std::strerror(errno);
            return false;
        }
        DecodedSandboxPacket packet{};
        if (!receivePacket(packet, 0, error)) {
            return false;
        }
        if (packet.header.opcode == SandboxOpcode::ProcessExited) {
            SandboxProcessExited exited{};
            if (!decodeSandboxProcessExited(packet.payload, exited)) {
                error = "invalid sandbox process-exit event";
                return false;
            }
            pendingExits_.push_back(exited);
            continue;
        }
        if (packet.header.requestId != requestId || packet.header.opcode != SandboxOpcode::LaunchResult ||
            !decodeSandboxLaunchResult(packet.payload, result)) {
            error = "invalid sandbox launch response";
            return false;
        }
        return true;
    }
}

bool SandboxClient::pollExit(SandboxProcessExited& event, std::string& error) {
    error.clear();
    if (!pendingExits_.empty()) {
        event = pendingExits_.front();
        pendingExits_.pop_front();
        return true;
    }
    if (descriptor_ < 0) {
        error = "not connected to lcl-sandboxd";
        return false;
    }
    DecodedSandboxPacket packet{};
    if (!receivePacket(packet, MSG_DONTWAIT, error)) {
        return false;
    }
    if (packet.header.opcode != SandboxOpcode::ProcessExited ||
        !decodeSandboxProcessExited(packet.payload, event)) {
        error = "sandboxd sent an unexpected control packet";
        return false;
    }
    return true;
}

} // namespace lcl::security
