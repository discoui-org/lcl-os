#include "system/ipc/raster_protocol.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
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

bool sendCommitTransaction(
        int fd, const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations, int displayListFd) {
    const bool hasDisplayList = transaction.displayListSize != 0;
    if (mutations.size() != transaction.mutationCount ||
        hasDisplayList != (displayListFd >= 0) ||
        mutations.size() >
            (kMaxPayload - sizeof(CommitTransaction)) / sizeof(NodeMutation)) {
        return false;
    }
    const size_t payloadSize = sizeof(CommitTransaction) +
        mutations.size() * sizeof(NodeMutation);
    if (payloadSize > std::numeric_limits<uint32_t>::max()) return false;
    std::vector<uint8_t> payload(payloadSize);
    std::memcpy(payload.data(), &transaction, sizeof(transaction));
    if (!mutations.empty()) {
        std::memcpy(payload.data() + sizeof(transaction), mutations.data(),
                    mutations.size_bytes());
    }
    return sendPacket(fd, Opcode::CommitTransaction, payload.data(),
                      static_cast<uint32_t>(payload.size()), displayListFd);
}

bool decodeCommitTransaction(
        const Header& header, std::span<const uint8_t> payload,
        CommitTransaction& transaction,
        std::vector<NodeMutation>& mutations) {
    mutations.clear();
    if (header.opcode != Opcode::CommitTransaction ||
        header.payloadSize != payload.size() ||
        payload.size() < sizeof(CommitTransaction)) {
        return false;
    }
    std::memcpy(&transaction, payload.data(), sizeof(transaction));
    const size_t available = payload.size() - sizeof(CommitTransaction);
    if (transaction.mutationCount >
            available / sizeof(NodeMutation) ||
        available != static_cast<size_t>(transaction.mutationCount) *
            sizeof(NodeMutation)) {
        return false;
    }
    mutations.resize(transaction.mutationCount);
    if (!mutations.empty()) {
        std::memcpy(mutations.data(), payload.data() + sizeof(transaction),
                    mutations.size() * sizeof(NodeMutation));
    }
    return true;
}

ReceiveStatus receivePacket(int fd, Header& header,
                            std::vector<uint8_t>& payload, int& receivedFd) {
    // receivePacket() is called from non-blocking drain loops, so the common
    // case is WouldBlock. Allocating and zero-filling kMaxPayload before every
    // recvmsg made an idle raster service churn through hundreds of MiB/s.
    // Keep one uninitialised receive slab per polling thread and copy only the
    // bytes that were actually delivered into the caller-owned payload.
    thread_local std::array<uint8_t, kMaxPayload> receivePayload{};
    payload.clear();
    receivedFd = -1;

    iovec vectors[2]{};
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = receivePayload.data();
    vectors[1].iov_len = receivePayload.size();

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
        return errno == EAGAIN || errno == EWOULDBLOCK
            ? ReceiveStatus::WouldBlock : ReceiveStatus::Error;
    }
    if (received == 0) {
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
        return ReceiveStatus::Invalid;
    }

    payload.assign(receivePayload.begin(),
                   receivePayload.begin() + header.payloadSize);
    return ReceiveStatus::Received;
}

} // namespace lcl::raster_protocol
