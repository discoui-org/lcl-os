#pragma once

#include <string>
#include <vector>

namespace lcl::core {

class IPCManager {
public:
    IPCManager();
    ~IPCManager();

    // Non-copyable
    IPCManager(const IPCManager&) = delete;
    IPCManager& operator=(const IPCManager&) = delete;

    /**
     * @brief Initialize FIFO / IPC pipe listener for compositor requests.
     * @param fifoPath Path to FIFO pipe (default: "/tmp/lcl_ipc.fifo")
     * @return true if created/initialized, false otherwise
     */
    bool initialize(const std::string& fifoPath = "/tmp/lcl_ipc.fifo");

    /**
     * @brief Read pending IPC messages non-blockingly and return command strings.
     */
    std::vector<std::string> pollMessages();

    /**
     * @brief Send an IPC message to the LCL OS Compositor.
     */
    static bool sendMessage(const std::string& message, const std::string& fifoPath = "/tmp/lcl_ipc.fifo");

    void shutdown();

    bool isInitialized() const { return m_initialized; }

private:
    std::string m_fifoPath;
    int m_fifoFd{-1};
    bool m_initialized{false};
};

} // namespace lcl::core
