#include "system/ipc/ipc_manager.hpp"
#include "system/security/desktop_user.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cstddef>
#include <cerrno>
#include <algorithm>
#include <filesystem>

namespace lcl::core {

IPCManager::IPCManager() = default;
IPCManager::~IPCManager() { shutdown(); }

bool IPCManager::initialize(const std::string& socketPath) {
    if (m_initialized) return true;
    m_socketPath = socketPath;

    const std::filesystem::path parent = std::filesystem::path(m_socketPath).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code error;
        if (!std::filesystem::create_directories(parent, error)) {
            std::cerr << "[LCL IPC ERROR] runtime directory creation failed for "
                      << parent << ": " << error.message() << "\n";
            return false;
        }
        chmod(parent.c_str(), 0700);
    }

    // Remove existing socket file if any
    unlink(m_socketPath.c_str());

    // Create Unix Domain Socket
    m_serverFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_serverFd < 0) {
        std::cerr << "[LCL IPC ERROR] socket creation failed: " << std::strerror(errno) << "\n";
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[LCL IPC ERROR] bind failed on " << m_socketPath << ": " << std::strerror(errno) << "\n";
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    // The root compositor creates this endpoint, but only the unprivileged
    // desktop session owns the 0600 client-facing socket.
    std::string ownershipError;
    if (!lcl::security::assignDesktopUserOwnership(m_socketPath, 0600, ownershipError)) {
        std::cerr << "[LCL IPC ERROR] " << ownershipError << "\n";
        close(m_serverFd);
        m_serverFd = -1;
        unlink(m_socketPath.c_str());
        return false;
    }

    if (listen(m_serverFd, 16) < 0) {
        std::cerr << "[LCL IPC ERROR] listen failed: " << std::strerror(errno) << "\n";
        close(m_serverFd);
        m_serverFd = -1;
        unlink(m_socketPath.c_str());
        return false;
    }

    m_initialized = true;
    std::cout << "[LCL IPC] Secure Unix Domain Socket server active on " << m_socketPath << " (permissions: 0600)\n";
    return true;
}

std::vector<IPCClientMessage> IPCManager::pollMessages() {
    std::vector<IPCClientMessage> messages;
    if (m_serverFd < 0)
        return messages;

    // 1. Accept new incoming client connections non-blockingly
    while (true) {
        int clientFd =
            accept4(m_serverFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd >= 0) {
            m_clientFds.push_back(clientFd);
        } else {
            break;
        }
    }

    // 2. Poll messages from active connected client sockets
    for (auto it = m_clientFds.begin(); it != m_clientFds.end();) {
        int fd = *it;
        bool fdAlive = true;

        // Drain ALL pending binary protocol messages from this fd in one shot
        while (true) {
            protocol::LCLHeader header{};
            std::vector<uint8_t> payload;
            int rFd = -1;

            const auto receiveStatus =
                protocol::recvPacketWithFd(fd, header, payload, rFd);
            if (receiveStatus == protocol::ReceiveStatus::Received) {
                struct ucred cred{};
                socklen_t len = sizeof(cred);
                getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);

                if (header.requestId == 0 ||
                    header.opcode == protocol::LCLOpcode::AckResponse) {
                    if (rFd >= 0)
                        close(rFd);
                    protocol::discardPendingWrites(fd);
                    close(fd);
                    fdAlive = false;
                    IPCClientMessage discMsg{};
                    discMsg.clientFd = fd;
                    discMsg.pid = cred.pid;
                    discMsg.uid = cred.uid;
                    discMsg.gid = cred.gid;
                    discMsg.disconnected = true;
                    messages.push_back(discMsg);
                    break;
                }

                IPCClientMessage msg;
                msg.clientFd = fd;
                msg.pid = cred.pid;
                msg.uid = cred.uid;
                msg.gid = cred.gid;
                msg.passedFd = rFd;
                msg.header = header;
                msg.payload = payload;

                messages.push_back(msg);
                // Continue draining more messages from this fd
                continue;
            }

            if (receiveStatus == protocol::ReceiveStatus::WouldBlock) {
                // No more data — fd is still alive, stop draining
                break;
            }

            if (receiveStatus == protocol::ReceiveStatus::Invalid &&
                header.magic == protocol::LCL_PROTOCOL_MAGIC &&
                header.version == protocol::LCL_PROTOCOL_VERSION &&
                header.requestId != 0) {
                protocol::LCLMsgAckResponse error{};
                error.status = 1;
                std::strncpy(error.message, "invalid protocol packet",
                             sizeof(error.message) - 1);
                protocol::LCLHeader response{};
                response.opcode = protocol::LCLOpcode::AckResponse;
                response.requestId = header.requestId;
                response.payloadSize = sizeof(error);
                protocol::sendMsgWithFd(fd, response, &error);
            }

            // Real error or EOF — fd is dead
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
            protocol::discardPendingWrites(fd);
            close(fd);
            fdAlive = false;

            IPCClientMessage discMsg{};
            discMsg.clientFd = fd;
            discMsg.pid = cred.pid;
            discMsg.uid = cred.uid;
            discMsg.gid = cred.gid;
            discMsg.disconnected = true;
            messages.push_back(discMsg);
            break;
        }

        if (!fdAlive) {
            it = m_clientFds.erase(it);
            continue;
        }

        ++it;
    }

    return messages;
}


void IPCManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL IPC] Shutting down Secure Unix Domain Socket server...\n";

    for (int fd : m_clientFds) {
        if (fd >= 0) {
            protocol::discardPendingWrites(fd);
            close(fd);
        }
    }
    m_clientFds.clear();

    if (m_serverFd >= 0) {
        close(m_serverFd);
        m_serverFd = -1;
    }
    unlink(m_socketPath.c_str());
    m_initialized = false;
}

} // namespace lcl::core
