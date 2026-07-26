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

    ssize_t n = recvmsg(socketFd, &msg, 0);
    if (n <= 0) return false;
    if (n < static_cast<ssize_t>(sizeof(LCLHeader))) return false;
    if (header.magic != LCL_PROTOCOL_MAGIC) return false;

    // Check if ancillary SCM_RIGHTS control message received
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int)) &&
        cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        std::memcpy(&receivedFd, CMSG_DATA(cmsg), sizeof(int));
    }

    // Read payload if specified
    payload.clear();
    if (header.payloadSize > 0) {
        payload.resize(header.payloadSize);
        ssize_t pBytes = read(socketFd, payload.data(), header.payloadSize);
        if (pBytes < static_cast<ssize_t>(header.payloadSize)) {
            return false;
        }
    }

    return true;
}

} // namespace lcl::protocol
