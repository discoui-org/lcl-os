#include "core/ipc/ipc_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <cerrno>

namespace lcl::core {

IPCManager::IPCManager() = default;
IPCManager::~IPCManager() { shutdown(); }

bool IPCManager::initialize(const std::string& fifoPath) {
    if (m_initialized) return true;
    m_fifoPath = fifoPath;

    // Remove existing FIFO if any
    unlink(m_fifoPath.c_str());

    // Create named FIFO pipe
    if (mkfifo(m_fifoPath.c_str(), 0666) < 0) {
        std::cerr << "[LCL IPC ERROR] mkfifo failed on " << m_fifoPath << ": " << std::strerror(errno) << "\n";
        return false;
    }

    // Open read/write non-blocking so read() never blocks
    m_fifoFd = open(m_fifoPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fifoFd < 0) {
        std::cerr << "[LCL IPC ERROR] open failed on " << m_fifoPath << ": " << std::strerror(errno) << "\n";
        unlink(m_fifoPath.c_str());
        return false;
    }

    m_initialized = true;
    std::cout << "[LCL IPC] Compositor IPC listener active on " << m_fifoPath << "\n";
    return true;
}

std::vector<std::string> IPCManager::pollMessages() {
    std::vector<std::string> messages;
    if (m_fifoFd < 0) return messages;

    char buf[512];
    std::string rawData;
    while (true) {
        ssize_t n = read(m_fifoFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            rawData.append(buf, n);
        } else {
            break;
        }
    }

    if (!rawData.empty()) {
        std::string cur;
        for (char c : rawData) {
            if (c == '\n') {
                if (!cur.empty()) {
                    messages.push_back(cur);
                    cur.clear();
                }
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) messages.push_back(cur);
    }

    return messages;
}

bool IPCManager::sendMessage(const std::string& message, const std::string& fifoPath) {
    int fd = open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    std::string msg = message + "\n";
    ssize_t n = write(fd, msg.data(), msg.size());
    close(fd);
    return n > 0;
}

void IPCManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL IPC] Shutting down Compositor IPC listener...\n";
    if (m_fifoFd >= 0) {
        close(m_fifoFd);
        m_fifoFd = -1;
    }
    unlink(m_fifoPath.c_str());
    m_initialized = false;
}

} // namespace lcl::core
