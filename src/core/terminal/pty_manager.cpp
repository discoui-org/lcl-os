#include "core/terminal/pty_manager.hpp"
#include <iostream>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>

namespace lcl::core {

PTYManager::PTYManager() = default;

PTYManager::~PTYManager() {
    shutdown();
}

bool PTYManager::spawnShell(const std::string& shellPath) {
    if (isAlive()) return true;

    // Open PTY master
    m_masterFd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m_masterFd < 0) {
        std::cerr << "[LCL PTY ERROR] posix_openpt failed: " << std::strerror(errno) << "\n";
        return false;
    }

    if (grantpt(m_masterFd) < 0 || unlockpt(m_masterFd) < 0) {
        std::cerr << "[LCL PTY ERROR] grantpt/unlockpt failed: " << std::strerror(errno) << "\n";
        close(m_masterFd);
        m_masterFd = -1;
        return false;
    }

    char* ptsName = ptsname(m_masterFd);
    if (!ptsName) {
        std::cerr << "[LCL PTY ERROR] ptsname failed.\n";
        close(m_masterFd);
        m_masterFd = -1;
        return false;
    }
    m_slaveName = ptsName;

    std::cout << "[LCL PTY] Created PTY pair (Master FD: " << m_masterFd << ", Slave: " << m_slaveName << ")\n";

    // Set default initial window size (24 rows x 64 cols)
    resizeWindow(64, 24);

    // Set master FD to non-blocking mode for non-blocking reads
    int flags = fcntl(m_masterFd, F_GETFL, 0);
    fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    // Fork child process
    m_childPid = fork();
    if (m_childPid < 0) {
        std::cerr << "[LCL PTY ERROR] fork failed: " << std::strerror(errno) << "\n";
        close(m_masterFd);
        m_masterFd = -1;
        return false;
    }

    if (m_childPid == 0) {
        // Child process
        close(m_masterFd);
        setsid();

        int slaveFd = open(m_slaveName.c_str(), O_RDWR);
        if (slaveFd >= 0) {
            #ifdef TIOCSCTTY
            ioctl(slaveFd, TIOCSCTTY, 0);
            #endif
            dup2(slaveFd, STDIN_FILENO);
            dup2(slaveFd, STDOUT_FILENO);
            dup2(slaveFd, STDERR_FILENO);
            if (slaveFd > STDERR_FILENO) close(slaveFd);
        }

        // Set HOME environment variable and change working directory to /home/user
        const char* userHome = "/home/user";
        struct stat st{};
        if (stat(userHome, &st) == 0 && S_ISDIR(st.st_mode)) {
            chdir(userHome);
            setenv("HOME", userHome, 1);
            setenv("PWD", userHome, 1);
        } else {
            setenv("HOME", "/", 1);
        }
        setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "\\W \xe2\x9d\xaf ", 1);
        setenv("BASH_ENV", "/home/user/.bashrc", 1);

        // Prefer bash for interactive use; fall back through specified shell to /bin/sh
        char* const bashArgv[] = {
            const_cast<char*>("/usr/bin/bash"),
            const_cast<char*>("--login"),
            const_cast<char*>("-i"),
            nullptr
        };
        execv("/usr/bin/bash", bashArgv);

        // Use caller-specified shell if bash not available
        if (!shellPath.empty() && shellPath != "/bin/sh") {
            char* const specArgv[] = {
                const_cast<char*>(shellPath.c_str()),
                const_cast<char*>("--login"),
                const_cast<char*>("-i"),
                nullptr
            };
            execv(shellPath.c_str(), specArgv);
        }

        // Last-resort fallback
        char* const shArgv[] = { const_cast<char*>("/bin/sh"), nullptr };
        execv("/bin/sh", shArgv);
        _exit(127);

    }

    std::cout << "[LCL PTY] Shell process spawned with PID: " << m_childPid << "\n";
    return true;
}

void PTYManager::resizeWindow(int cols, int rows) {
    if (m_masterFd < 0) return;
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(cols * 8);
    ws.ws_ypixel = static_cast<unsigned short>(rows * 16);
    ioctl(m_masterFd, TIOCSWINSZ, &ws);
}

bool PTYManager::writeInput(const std::string& input) {
    if (m_masterFd < 0 || input.empty()) return false;
    ssize_t n = write(m_masterFd, input.data(), input.size());
    return n > 0;
}

std::string PTYManager::readOutput() {
    if (m_masterFd < 0) return "";

    char buf[1024];
    std::string result;
    while (true) {
        ssize_t n = read(m_masterFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            result.append(buf, n);
        } else {
            break; // EWOULDBLOCK or EOF
        }
    }
    return result;
}

std::string PTYManager::getWorkingDirectory() const {
    if (m_childPid <= 0) return "/home/user";

    std::string procPath = "/proc/" + std::to_string(m_childPid) + "/cwd";
    char buf[1024];
    ssize_t len = readlink(procPath.c_str(), buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "/home/user";
}

void PTYManager::shutdown() {
    if (m_childPid > 0) {
        std::cout << "[LCL PTY] Terminating shell process group PGID: " << m_childPid << "...\n";
        kill(-m_childPid, SIGHUP);
        kill(-m_childPid, SIGTERM);
        usleep(10000);
        if (kill(m_childPid, 0) == 0) {
            kill(-m_childPid, SIGKILL);
        }
        waitpid(m_childPid, nullptr, WNOHANG);
        m_childPid = -1;
    }
    if (m_masterFd >= 0) {
        close(m_masterFd);
        m_masterFd = -1;
    }
}

} // namespace lcl::core
