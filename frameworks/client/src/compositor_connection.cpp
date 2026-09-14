#include "lcl-client/compositor_connection.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace lcl::client {

CompositorConnection::~CompositorConnection() {
    disconnect();
}

bool CompositorConnection::adopt(int socketFd, Ownership ownership) {
    if (socketFd < 0) return false;
    disconnect();
    const int flags = fcntl(socketFd, F_GETFL, 0);
    const int descriptorFlags = fcntl(socketFd, F_GETFD, 0);
    if (flags < 0 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    if (descriptorFlags >= 0) {
        (void)fcntl(socketFd, F_SETFD, descriptorFlags | FD_CLOEXEC);
    }
    m_fd = socketFd;
    m_owned = ownership == Ownership::Owned;
    return true;
}

bool CompositorConnection::connect(const std::string& socketPath,
                                   uint32_t attempts,
                                   uint32_t retryDelayMs) {
    disconnect();
    sockaddr_un address{};
    if (socketPath.empty() || attempts == 0 ||
        socketPath.size() >= sizeof(address.sun_path)) return false;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        const int socketFd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (socketFd >= 0) {
            address = {};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, socketPath.c_str(),
                        socketPath.size() + 1);
            if (::connect(socketFd, reinterpret_cast<sockaddr*>(&address),
                          sizeof(address)) == 0 &&
                adopt(socketFd, Ownership::Owned)) {
                return true;
            }
            close(socketFd);
        }
        if (attempt + 1 < attempts && retryDelayMs != 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(retryDelayMs));
        }
    }
    return false;
}

void CompositorConnection::disconnect() noexcept {
    if (m_fd >= 0 && m_owned) {
        protocol::discardPendingWrites(m_fd);
        close(m_fd);
    }
    m_fd = -1;
    m_owned = false;
}

bool CompositorConnection::send(protocol::LCLOpcode opcode,
                                uint32_t requestId, const void* payload,
                                uint32_t payloadSize, int passedFd) {
    if (m_fd < 0) return false;
    protocol::LCLHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = payloadSize;
    return protocol::sendMsgWithFd(m_fd, header, payload, passedFd);
}

protocol::ReceiveStatus CompositorConnection::receive(
        protocol::LCLHeader& header, std::vector<uint8_t>& payload,
        int& receivedFd) {
    if (m_fd < 0) return protocol::ReceiveStatus::Closed;
    return protocol::recvPacketWithFd(m_fd, header, payload, receivedFd);
}

} // namespace lcl::client
