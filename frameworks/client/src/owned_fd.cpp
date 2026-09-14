#include "lcl-client/owned_fd.hpp"

#include <unistd.h>

namespace lcl::client {

OwnedFd::~OwnedFd() {
    reset();
}

OwnedFd::OwnedFd(OwnedFd&& other) noexcept : m_fd(other.release()) {}

OwnedFd& OwnedFd::operator=(OwnedFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
}

int OwnedFd::release() noexcept {
    const int fd = m_fd;
    m_fd = -1;
    return fd;
}

void OwnedFd::reset(int fd) noexcept {
    if (m_fd >= 0) close(m_fd);
    m_fd = fd;
}

} // namespace lcl::client
