#include "system/ipc/gpu_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::gpu_protocol {

bool sendPacket(int socketFd, Opcode opcode, const void* payload,
                uint32_t payloadSize) {
    return sendPacketWithFd(socketFd, opcode, payload, payloadSize, -1);
}

bool sendPacketWithFd(int socketFd, Opcode opcode, const void* payload,
                      uint32_t payloadSize, int descriptor) {
    if (socketFd < 0 || payloadSize > kMaxPayload ||
        (payloadSize != 0 && !payload)) {
        return false;
    }
    Header header{};
    header.opcode = opcode;
    header.payloadSize = payloadSize;
    iovec vectors[2]{{&header, sizeof(header)},
                     {const_cast<void*>(payload), payloadSize}};
    msghdr message{};
    message.msg_iov = vectors;
    message.msg_iovlen = payloadSize == 0 ? 1 : 2;
    char control[CMSG_SPACE(sizeof(int))]{};
    if (descriptor >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    }
    const ssize_t written = sendmsg(socketFd, &message, MSG_NOSIGNAL);
    return written == static_cast<ssize_t>(sizeof(header) + payloadSize);
}

ReceiveStatus receivePacket(int socketFd, Header& header,
                            std::vector<uint8_t>& payload) {
    int descriptor = -1;
    const ReceiveStatus status = receivePacketWithFd(socketFd, header, payload, descriptor);
    if (descriptor >= 0) close(descriptor);
    return status;
}

ReceiveStatus receivePacketWithFd(int socketFd, Header& header,
                                  std::vector<uint8_t>& payload,
                                  int& descriptor) {
    header = {};
    payload.clear();
    descriptor = -1;
    if (socketFd < 0) return ReceiveStatus::Error;

    std::vector<uint8_t> bytes(sizeof(Header) + kMaxPayload);
    iovec vector{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = recvmsg(socketFd, &message, MSG_DONTWAIT);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReceiveStatus::WouldBlock;
        }
        return ReceiveStatus::Error;
    }
    if (received == 0) return ReceiveStatus::Closed;
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        received < static_cast<ssize_t>(sizeof(Header))) {
        return ReceiveStatus::Invalid;
    }

    for (cmsghdr* current = CMSG_FIRSTHDR(&message); current;
         current = CMSG_NXTHDR(&message, current)) {
        if (current->cmsg_level != SOL_SOCKET || current->cmsg_type != SCM_RIGHTS ||
            current->cmsg_len != CMSG_LEN(sizeof(int)) || descriptor >= 0) {
            // recvmsg() installs SCM_RIGHTS descriptors before we validate the
            // complete ancillary payload.  Never leak one on a malformed
            // multi-cmsg packet.
            if (descriptor >= 0) {
                close(descriptor);
                descriptor = -1;
            }
            return ReceiveStatus::Invalid;
        }
        std::memcpy(&descriptor, CMSG_DATA(current), sizeof(descriptor));
    }

    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMagic || header.version != kVersion ||
        header.payloadSize > kMaxPayload ||
        received != static_cast<ssize_t>(sizeof(Header) + header.payloadSize)) {
        if (descriptor >= 0) {
            close(descriptor);
            descriptor = -1;
        }
        return ReceiveStatus::Invalid;
    }
    payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(Header)),
                   bytes.begin() + received);
    return ReceiveStatus::Received;
}

} // namespace lcl::gpu_protocol
