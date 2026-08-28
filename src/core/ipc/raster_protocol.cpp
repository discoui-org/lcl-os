#include "core/ipc/raster_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::raster_protocol {

bool sendPacket(int fd, Opcode opcode, const void* payload,
                uint32_t payloadSize, int passedFd) {
    if (fd < 0 || payloadSize > kMaxPayload || (payloadSize != 0 && !payload)) {
        return false;
    }

    Header header{};
    header.opcode = opcode;
    header.payloadSize = payloadSize;

    iovec vectors[2]{};
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = const_cast<void*>(payload);
    vectors[1].iov_len = payloadSize;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = payloadSize == 0 ? 1 : 2;
    if (passedFd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &passedFd, sizeof(int));
    }

    const auto expected = static_cast<ssize_t>(sizeof(header) + payloadSize);
    ssize_t sent = -1;
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == expected;
}

ReceiveStatus receivePacket(int fd, Header& header,
                            std::vector<uint8_t>& payload, int& receivedFd) {
    payload.assign(kMaxPayload, 0);
    receivedFd = -1;

    iovec vectors[2]{};
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = payload.data();
    vectors[1].iov_len = payload.size();

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = 2;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    ssize_t received = -1;
    do {
        received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        payload.clear();
        return errno == EAGAIN || errno == EWOULDBLOCK
            ? ReceiveStatus::WouldBlock : ReceiveStatus::Error;
    }
    if (received == 0) {
        payload.clear();
        return ReceiveStatus::Closed;
    }

    for (auto* cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            std::memcpy(&receivedFd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }

    const bool validHeader = received >= static_cast<ssize_t>(sizeof(Header)) &&
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
        header.magic == kMagic && header.version == kVersion &&
        header.payloadSize <= kMaxPayload &&
        received == static_cast<ssize_t>(sizeof(Header) + header.payloadSize);
    if (!validHeader) {
        if (receivedFd >= 0) close(receivedFd);
        receivedFd = -1;
        payload.clear();
        return ReceiveStatus::Invalid;
    }

    payload.resize(header.payloadSize);
    return ReceiveStatus::Received;
}

} // namespace lcl::raster_protocol
