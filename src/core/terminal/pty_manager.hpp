#pragma once

#include <string>
#include <sys/types.h>
#include <signal.h>

namespace lcl::core {

class PTYManager {
public:
    PTYManager();
    ~PTYManager();

    // Non-copyable
    PTYManager(const PTYManager&) = delete;
    PTYManager& operator=(const PTYManager&) = delete;

    /**
     * @brief Spawn shell binary inside pseudo-terminal (PTY master/slave).
     * @param shellPath Path to shell executable (default: "/bin/sh")
     */
    bool spawnShell(const std::string& shellPath = "/bin/sh");

    /**
     * @brief Read pending output from PTY master.
     */
    std::string readOutput();

    /**
     * @brief Write input data into PTY master.
     */
    bool writeInput(const std::string& input);

    /**
     * @brief Update window dimensions for PTY TIOCSWINSZ.
     */
    void resizeWindow(int cols, int rows);

    /**
     * @brief Terminate shell process and close PTY descriptors.
     */
    void shutdown();

    bool isInitialized() const { return m_masterFd >= 0; }
    int getMasterFd() const { return m_masterFd; }
    pid_t getChildPid() const { return m_childPid; }

    bool isAlive() const {
        if (m_childPid <= 0) return false;
        return kill(m_childPid, 0) == 0;
    }

private:
    int m_masterFd{-1};
    pid_t m_childPid{-1};
    std::string m_slaveName;
};

} // namespace lcl::core
