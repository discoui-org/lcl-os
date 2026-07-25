#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unistd.h>

namespace lcl::core {

class PTYManager {
public:
    PTYManager();
    ~PTYManager();

    // Non-copyable
    PTYManager(const PTYManager&) = delete;
    PTYManager& operator=(const PTYManager&) = delete;

    /**
     * @brief Spawn shell process (/bin/sh or /bin/bash) connected to a PTY master/slave pair.
     * @param shellPath Path to shell binary (default: "/bin/sh")
     * @return true if process spawned, false otherwise
     */
    bool spawnShell(const std::string& shellPath = "/bin/sh");

    /**
     * @brief Update PTY window size (rows and columns).
     */
    void setWindowSize(int rows, int cols);

    /**
     * @brief Write user input data (keystrokes) to PTY master.
     */
    ssize_t writeInput(const std::string& data);

    /**
     * @brief Read available output bytes from PTY master.
     */
    std::string readOutput();

    /**
     * @brief Terminate shell process and close PTY.
     */
    void shutdown();

    bool isRunning() const { return m_masterFd >= 0 && m_childPid > 0; }
    int getMasterFd() const { return m_masterFd; }

private:
    int m_masterFd{-1};
    pid_t m_childPid{-1};
    std::string m_slaveName;
};

} // namespace lcl::core
