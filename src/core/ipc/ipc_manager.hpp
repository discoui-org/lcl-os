#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace lcl::core {

struct IPCClientMessage {
    int clientFd{-1};
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
    std::string command;
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
     * @param socketPath Path to Unix domain socket (default: "/tmp/lcl_compositor.sock")
     * @return true if created and listening with 0600 permissions, false otherwise
     */
    bool initialize(const std::string& socketPath = "/tmp/lcl_compositor.sock");

    /**
     * @brief Poll non-blocking client connections & incoming authenticated IPC messages.
     */
    std::vector<IPCClientMessage> pollMessages();

    /**
     * @brief Send a response to a specific connected client socket.
     */
    static bool sendResponse(int clientFd, const std::string& response);

    /**
     * @brief Send an IPC request to Compositor server as a client.
     * @param request Message string to send
     * @param socketPath Path to Unix domain socket
     * @param waitResponse If true, wait for server response string and return it
     */
    static std::string sendClientRequest(const std::string& request, const std::string& socketPath = "/tmp/lcl_compositor.sock", bool waitResponse = false);

    void shutdown();

    bool isInitialized() const { return m_initialized; }

private:
    std::string m_socketPath;
    int m_serverFd{-1};
    std::vector<int> m_clientFds;
    bool m_initialized{false};
};

} // namespace lcl::core
