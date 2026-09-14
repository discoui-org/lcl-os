#pragma once

namespace lcl::client {

/** Move-only ownership for a POSIX file descriptor. */
class OwnedFd {
public:
    OwnedFd() noexcept = default;
    explicit OwnedFd(int fd) noexcept : m_fd(fd) {}
    ~OwnedFd();

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept;
    OwnedFd& operator=(OwnedFd&& other) noexcept;

    int get() const noexcept { return m_fd; }
    explicit operator bool() const noexcept { return m_fd >= 0; }
    int release() noexcept;
    void reset(int fd = -1) noexcept;

private:
    int m_fd{-1};
};

} // namespace lcl::client
