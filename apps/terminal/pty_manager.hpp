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

    /**
     * @brief Retrieve current working directory path of child shell process via /proc/<pid>/cwd.
     */
    std::string getWorkingDirectory() const;

    /**
     * @brief Reap an exited shell without blocking the UI event loop.
     * @return True when the child has exited or was already reaped.
     */
    bool pollChildExit();

    /**
     * @brief Return whether the shell is still running.
     *
     * This also reaps an exited child so a zombie cannot keep the terminal
     * window alive after the user runs `exit`.
     */
    bool isAlive();

private:
    int m_masterFd{-1};
    pid_t m_childPid{-1};
    std::string m_slaveName;
    int m_lastCols{-1};
    int m_lastRows{-1};
};

} // namespace lcl::core
