#include "system/security/administrator_client.hpp"

#include "system/security/session_user.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>

namespace lcl::security {
namespace {

bool connectRootDaemon(const std::string& socketPath, int& descriptor,
                       std::string& error) {
    error.clear();
    if (socketPath.empty() || socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "administrator socket path is invalid";
        return false;
    }
    const int candidate = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (candidate < 0) {
        error = std::strerror(errno);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(candidate, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::strerror(errno);
        ::close(candidate);
        return false;
    }
    ucred peer{};
    socklen_t peerSize = sizeof(peer);
    struct stat status {};
    if (getsockopt(candidate, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0 ||
        peerSize != sizeof(peer) || peer.uid != 0 || fstat(candidate, &status) != 0 ||
        !S_ISSOCK(status.st_mode)) {
        error = "administrator endpoint is not owned by the root broker";
        ::close(candidate);
        return false;
    }
    descriptor = candidate;
    return true;
}

bool sendEncodedPacket(int descriptor, AdministratorOpcode opcode,
                       std::uint32_t requestId,
                       const std::vector<std::uint8_t>& payload) {
    AdministratorHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    return encodeAdministratorPacket(header, payload, packet) &&
           send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(packet.size());
}

} // namespace

AdministratorPromptClient::~AdministratorPromptClient() { close(); }

bool AdministratorPromptClient::connect(const std::string& socketPath) {
    close();
    if (geteuid() != kSessionUserUid || getegid() != kSessionUserGid) return false;
    std::string error;
    if (!connectRootDaemon(socketPath, descriptor_, error) ||
        !sendPacket(AdministratorOpcode::ShellHello, 1, {})) {
        close();
        return false;
    }
    const int flags = fcntl(descriptor_, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) != 0) {
        close();
        return false;
    }
    return true;
}

void AdministratorPromptClient::close() {
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = -1;
}

bool AdministratorPromptClient::sendPacket(
        AdministratorOpcode opcode, std::uint32_t requestId,
        const std::vector<std::uint8_t>& payload) {
    return descriptor_ >= 0 &&
           sendEncodedPacket(descriptor_, opcode, requestId, payload);
}

bool AdministratorPromptClient::poll(std::uint32_t& requestId,
                                     AdministratorPrompt& prompt) {
    if (descriptor_ < 0) return false;
    std::array<std::uint8_t,
               kAdministratorWireHeaderSize + kAdministratorMaxPayload> bytes{};
    const ssize_t count = recv(descriptor_, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return false;
    }
    DecodedAdministratorPacket packet{};
    if (count <= 0 ||
        !decodeAdministratorPacket(bytes.data(), static_cast<std::size_t>(count), packet) ||
        packet.header.opcode != AdministratorOpcode::PermissionPrompt ||
        !decodeAdministratorPrompt(packet.payload, prompt)) {
        close();
        return false;
    }
    requestId = packet.header.requestId;
    return true;
}

bool AdministratorPromptClient::decide(std::uint32_t requestId, bool allowed) {
    std::vector<std::uint8_t> payload;
    if (!encodeAdministratorDecision(allowed, payload) ||
        !sendPacket(AdministratorOpcode::PermissionDecision, requestId, payload)) {
        close();
        return false;
    }
    return true;
}

AdministratorCommandClient::~AdministratorCommandClient() { close(); }

bool AdministratorCommandClient::connect(const std::string& socketPath,
                                          std::string& error) {
    close();
    return connectRootDaemon(socketPath, descriptor_, error);
}

void AdministratorCommandClient::close() {
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = -1;
}

bool AdministratorCommandClient::sendPacket(
        AdministratorOpcode opcode, std::uint32_t requestId,
        const std::vector<std::uint8_t>& payload, std::string& error) {
    if (descriptor_ < 0 || !sendEncodedPacket(descriptor_, opcode, requestId, payload)) {
        error = descriptor_ < 0 ? "lcl-admind is not connected" : std::strerror(errno);
        return false;
    }
    return true;
}

bool AdministratorCommandClient::receivePacket(DecodedAdministratorPacket& packet,
                                                std::string& error) {
    std::array<std::uint8_t,
               kAdministratorWireHeaderSize + kAdministratorMaxPayload> bytes{};
    const ssize_t count = recv(descriptor_, bytes.data(), bytes.size(), 0);
    if (count <= 0 ||
        !decodeAdministratorPacket(bytes.data(), static_cast<std::size_t>(count), packet)) {
        error = count == 0 ? "lcl-admind closed the request" :
                (count < 0 ? std::strerror(errno) : "invalid lcl-admind response");
        return false;
    }
    return true;
}

int AdministratorCommandClient::execute(const AdministratorExecuteRequest& request,
                                        std::string& error) {
    std::vector<std::uint8_t> payload;
    if (!encodeAdministratorExecuteRequest(request, payload)) {
        error = "administrator command is invalid";
        return 125;
    }
    const std::uint32_t requestId = nextRequestId_++;
    if (!sendPacket(AdministratorOpcode::ExecuteRequest, requestId, payload, error)) {
        return 125;
    }
    while (true) {
        DecodedAdministratorPacket packet{};
        if (!receivePacket(packet, error) || packet.header.requestId != requestId) {
            if (error.empty()) error = "mismatched lcl-admind response";
            return 125;
        }
        if (packet.header.opcode == AdministratorOpcode::CommandOutput) {
            AdministratorCommandOutput output{};
            if (!decodeAdministratorCommandOutput(packet.payload, output)) {
                error = "invalid lcl-admind output";
                return 125;
            }
            const int target = output.stream == AdministratorOutputStream::StandardError
                ? STDERR_FILENO : STDOUT_FILENO;
            std::size_t offset = 0;
            while (offset < output.bytes.size()) {
                const ssize_t written = write(target, output.bytes.data() + offset,
                                              output.bytes.size() - offset);
                if (written > 0) {
                    offset += static_cast<std::size_t>(written);
                } else if (written < 0 && errno == EINTR) {
                    continue;
                } else {
                    error = "could not write administrator command output";
                    return 125;
                }
            }
            continue;
        }
        if (packet.header.opcode == AdministratorOpcode::CommandResult) {
            int exitCode = 0;
            if (!decodeAdministratorCommandResult(packet.payload, exitCode)) {
                error = "invalid lcl-admind command result";
                return 125;
            }
            return exitCode;
        }
        if (packet.header.opcode == AdministratorOpcode::ErrorResponse) {
            if (!decodeAdministratorError(packet.payload, error)) {
                error = "invalid lcl-admind error response";
            }
            return 126;
        }
        error = "unexpected lcl-admind response";
        return 125;
    }
}

} // namespace lcl::security
