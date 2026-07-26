#include "core/ipc/lcl_protocol.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace lcl::protocol {

bool sendMsgWithFd(int socketFd, const LCLHeader& header, const void* payload, int passedFd) {
    if (socketFd < 0) return false;

    struct msghdr msg{};
    struct iovec iov[2];

    iov[0].iov_base = const_cast<LCLHeader*>(&header);
    iov[0].iov_len = sizeof(LCLHeader);

    iov[1].iov_base = const_cast<void*>(payload);
    iov[1].iov_len = header.payloadSize;

    msg.msg_iov = iov;
    msg.msg_iovlen = (payload && header.payloadSize > 0) ? 2 : 1;

    union {
        struct cmsghdr cmsghdr;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsgu;

    if (passedFd >= 0) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        std::memcpy(CMSG_DATA(cmsg), &passedFd, sizeof(int));
    }

    ssize_t n = sendmsg(socketFd, &msg, 0);
    return n > 0;
}

bool recvMsgWithFd(int socketFd, LCLHeader& header, std::vector<uint8_t>& payload, int& receivedFd) {
    receivedFd = -1;
    if (socketFd < 0) return false;

    struct msghdr msg{};
    struct iovec iov[1];

    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(LCLHeader);

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        struct cmsghdr cmsghdr;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsgu;

    msg.msg_control = cmsgu.control;
    msg.msg_controllen = sizeof(cmsgu.control);

    // Use MSG_DONTWAIT so non-blocking sockets return EAGAIN cleanly
    ssize_t n = recvmsg(socketFd, &msg, MSG_DONTWAIT);
    if (n <= 0) return false;  // includes EAGAIN (errno set), closed, or error
    if (n < static_cast<ssize_t>(sizeof(LCLHeader))) return false;
    if (header.magic != LCL_PROTOCOL_MAGIC) return false;

    // Check if ancillary SCM_RIGHTS control message received
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int)) &&
        cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        std::memcpy(&receivedFd, CMSG_DATA(cmsg), sizeof(int));
    }

    // Read payload if specified — use blocking read since header already received
    payload.clear();
    if (header.payloadSize > 0 && header.payloadSize < 65536) {
        payload.resize(header.payloadSize);
        size_t bytesRead = 0;
        while (bytesRead < header.payloadSize) {
            ssize_t pBytes = read(socketFd, payload.data() + bytesRead, header.payloadSize - bytesRead);
            if (pBytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Short spin: payload immediately follows header in the same send
                    continue;
                }
                return false;
            }
            bytesRead += static_cast<size_t>(pBytes);
        }
    }

    return true;
}

} // namespace lcl::protocol
