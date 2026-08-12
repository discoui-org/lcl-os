#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "core/ipc/lcl_protocol.hpp"

namespace lcl::core {

/// Canonical path for the Compositor Unix Domain Socket.
/// Used only by the compositor and client surface connections. Application
/// catalog and launch lifecycle use the separate lcl-sessiond socket.
inline constexpr const char* kCompositorSocket = "/run/user/1000/lcl-compositor.sock";

struct IPCClientMessage {
    int clientFd{-1};
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
    bool disconnected{false};
    int passedFd{-1};
    lcl::protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
};

class IPCManager {
public:
    IPCManager();
    ~IPCManager();

    // Non-copyable
    IPCManager(const IPCManager&) = delete;
    IPCManager& operator=(const IPCManager&) = delete;

    /**
     * @brief Initialize Unix Domain Socket server listener for compositor requests.
     * @param socketPath Path to Unix domain socket (default: kCompositorSocket)
     * @return true if created and listening with 0600 permissions, false otherwise
     */
    bool initialize(const std::string& socketPath = kCompositorSocket);

    /**
     * @brief Poll non-blocking client connections & incoming authenticated IPC messages.
     */
    std::vector<IPCClientMessage> pollMessages();

    void shutdown();

    bool isInitialized() const { return m_initialized; }

private:
    std::string m_socketPath;
    int m_serverFd{-1};
    std::vector<int> m_clientFds;
    bool m_initialized{false};
};

} // namespace lcl::core
