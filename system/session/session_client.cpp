#include "system/session/session_client.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace lcl::session {

SessionClient::~SessionClient() {
    if (m_fd >= 0) close(m_fd);
}

bool SessionClient::connect(const std::string& socketPath) {
    if (m_fd >= 0) return true;
    m_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (m_fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        close(m_fd);
        m_fd = -1;
        errno = ENAMETOOLONG;
        return false;
    }
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(m_fd);
        m_fd = -1;
        return false;
    }
    return true;
}

bool SessionClient::sendRequest(SessionOpcode opcode, const std::vector<uint8_t>& payload,
                                uint32_t& requestId, std::string& error) {
    if (m_fd < 0) {
        error = "not connected to lcl-sessiond";
        return false;
    }
    requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    SessionHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> packet;
    if (!encodePacket(header, payload, packet) ||
        send(m_fd, packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size())) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool SessionClient::receive(DecodedPacket& packet, std::string& error) {
    std::array<uint8_t, kSessionWireHeaderSize + kSessionMaxPayload> bytes{};
    const ssize_t count = recv(m_fd, bytes.data(), bytes.size(), 0);
    if (count <= 0 || !decodePacket(bytes.data(), static_cast<size_t>(count), packet)) {
        error = count < 0 ? std::strerror(errno) : "sessiond closed the connection";
        return false;
    }
    if (packet.header.opcode == SessionOpcode::ErrorResponse) {
        decodeError(packet.payload, error);
        return false;
    }
    return true;
}

bool SessionClient::requestCatalog(std::vector<CatalogEntry>& entries, std::string& error) {
    uint32_t requestId = 0;
    if (!sendRequest(SessionOpcode::CatalogRequest, {}, requestId, error)) return false;
    DecodedPacket packet;
    return receive(packet, error) && packet.header.requestId == requestId &&
           packet.header.opcode == SessionOpcode::CatalogSnapshot &&
           decodeCatalogSnapshot(packet.payload, entries);
}

bool SessionClient::launch(const LaunchRequest& request, LaunchResponse& response,
                           std::string& error) {
    std::vector<uint8_t> payload;
    if (!encodeLaunchRequest(request, payload)) {
        error = "invalid launch request";
        return false;
    }
    uint32_t requestId = 0;
    if (!sendRequest(SessionOpcode::LaunchRequest, payload, requestId, error)) return false;
    DecodedPacket packet;
    if (!receive(packet, error) || packet.header.requestId != requestId ||
        packet.header.opcode != SessionOpcode::LaunchResponse ||
        !decodeLaunchResponse(packet.payload, response)) {
        if (error.empty()) error = "invalid launch response";
        return false;
    }
    if (response.status != 0) {
        error = response.message;
        return false;
    }
    return true;
}

bool SessionClient::waitForExit(uint64_t instanceId, ProcessExited& event,
                                std::string& error) {
    DecodedPacket packet;
    if (!receive(packet, error) || packet.header.opcode != SessionOpcode::ProcessExited ||
        !decodeProcessExited(packet.payload, event) || event.instanceId != instanceId) {
        if (error.empty()) error = "invalid process-exit event";
        return false;
    }
    return true;
}

} // namespace lcl::session
