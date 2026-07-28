#include "core/ipc/ipc_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cerrno>
#include <algorithm>

namespace lcl::core {

IPCManager::IPCManager() = default;
IPCManager::~IPCManager() { shutdown(); }

bool IPCManager::initialize(const std::string& socketPath) {
    if (m_initialized) return true;
    m_socketPath = socketPath;

    // Remove existing socket file if any
    unlink(m_socketPath.c_str());

    // Create Unix Domain Socket
    m_serverFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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

    // Set strict owner-only file permissions (0600) for IPC security
    chmod(m_socketPath.c_str(), 0600);

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
    if (m_serverFd < 0) return messages;

    // 1. Accept new incoming client connections non-blockingly
    while (true) {
        int clientFd = accept4(m_serverFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd >= 0) {
            m_clientFds.push_back(clientFd);
        } else {
            break;
        }
    }

    // 2. Poll messages from active connected client sockets
    for (auto it = m_clientFds.begin(); it != m_clientFds.end(); ) {
        int fd = *it;
        bool fdAlive = true;

        // Drain ALL pending binary protocol messages from this fd in one shot
        while (true) {
            protocol::LCLHeader header{};
            std::vector<uint8_t> payload;
            int rFd = -1;

            if (protocol::recvMsgWithFd(fd, header, payload, rFd)) {
                struct ucred cred{};
                socklen_t len = sizeof(cred);
                getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);

                IPCClientMessage msg;
                msg.clientFd = fd;
                msg.pid = cred.pid;
                msg.uid = cred.uid;
                msg.gid = cred.gid;
                msg.passedFd = rFd;
                msg.header = header;
                msg.payload = payload;

                if (header.opcode == protocol::LCLOpcode::RegisterRole && payload.size() >= sizeof(protocol::LCLMsgRegisterRole)) {
                    auto* reg = reinterpret_cast<const protocol::LCLMsgRegisterRole*>(payload.data());
                    msg.command = "REGISTER_ROLE:" + std::string(reg->clientName);
                } else if (header.opcode == protocol::LCLOpcode::SurfaceCreate && payload.size() >= sizeof(protocol::LCLMsgSurfaceCreate)) {
                    auto* surf = reinterpret_cast<const protocol::LCLMsgSurfaceCreate*>(payload.data());
                    msg.command = "SURFACE_CREATE:" + std::to_string(surf->surfaceId);
                } else if (header.opcode == protocol::LCLOpcode::AttachBuffer && payload.size() >= sizeof(protocol::LCLMsgAttachBuffer)) {
                    auto* buf = reinterpret_cast<const protocol::LCLMsgAttachBuffer*>(payload.data());
                    msg.command = "ATTACH_BUFFER:" + std::to_string(buf->surfaceId);
                } else {
                    msg.command = "PROTOCOL_OPCODE_" + std::to_string(static_cast<uint32_t>(header.opcode));
                }

                messages.push_back(msg);
                // Continue draining more messages from this fd
                continue;
            }

            // recvMsgWithFd returned false: check why
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data — fd is still alive, stop draining
                break;
            }

            // Real error or EOF — fd is dead
            close(fd);
            fdAlive = false;
            break;
        }

        if (!fdAlive) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);

            IPCClientMessage discMsg{};
            discMsg.clientFd = fd;
            discMsg.pid = cred.pid;
            discMsg.uid = cred.uid;
            discMsg.gid = cred.gid;
            discMsg.command = "CLIENT_DISCONNECT";
            discMsg.header.opcode = protocol::LCLOpcode::SurfaceDestroy;
            messages.push_back(discMsg);

            it = m_clientFds.erase(it);
            continue;
        }

        ++it;
    }

    return messages;
}

bool IPCManager::sendResponse(int clientFd, const std::string& response) {
    if (clientFd < 0) return false;
    std::string msg = response + "\n";
    ssize_t n = write(clientFd, msg.data(), msg.size());
    return n > 0;
}

std::string IPCManager::sendClientRequest(const std::string& request, const std::string& socketPath, bool waitResponse) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    std::string msg = request + "\n";
    if (write(fd, msg.data(), msg.size()) <= 0) {
        close(fd);
        return "";
    }

    std::string response;
    if (waitResponse) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            response = std::string(buf, n);
            // Trim newline
            if (!response.empty() && response.back() == '\n') response.pop_back();
        }
    }

    close(fd);
    return response;
}

void IPCManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL IPC] Shutting down Secure Unix Domain Socket server...\n";

    for (int fd : m_clientFds) {
        if (fd >= 0) close(fd);
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
