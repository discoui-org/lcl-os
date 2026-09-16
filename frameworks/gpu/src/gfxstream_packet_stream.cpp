#include "lcl-gpu/gfxstream_packet_stream.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>

namespace lcl::gpu {
namespace {

bool setNonBlocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool waitFor(int descriptor, short events) {
    pollfd ready{descriptor, events, 0};
    int result = -1;
    do {
        result = poll(&ready, 1, -1);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (ready.revents & events) != 0;
}

} // namespace

GfxstreamPacketStream::GfxstreamPacketStream(client::OwnedFd descriptor) noexcept
    : descriptor_(std::move(descriptor)) {
    if (descriptor_ && !setNonBlocking(descriptor_.get())) {
        descriptor_.reset();
    }
}

bool GfxstreamPacketStream::valid() const noexcept {
    return static_cast<bool>(descriptor_);
}

int GfxstreamPacketStream::fd() const noexcept {
    return descriptor_.get();
}

void GfxstreamPacketStream::close() noexcept {
    descriptor_.reset();
    unread_.clear();
    unreadOffset_ = 0;
}

bool GfxstreamPacketStream::writeFully(const void* bytes, std::size_t count) {
    if (!valid() || (!bytes && count != 0)) return false;
    const auto* source = static_cast<const std::uint8_t*>(bytes);
    while (count != 0) {
        const std::size_t fragment = std::min(count, kMaximumPacketBytes);
        ssize_t written = -1;
        do {
            written = send(descriptor_.get(), source, fragment, MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written == static_cast<ssize_t>(fragment)) {
            source += fragment;
            count -= fragment;
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            waitFor(descriptor_.get(), POLLOUT)) {
            continue;
        }
        close();
        return false;
    }
    return true;
}

bool GfxstreamPacketStream::refill() {
    if (!valid()) return false;
    std::array<std::uint8_t, kMaximumPacketBytes> packet{};
    for (;;) {
        iovec buffer{packet.data(), packet.size()};
        msghdr message{};
        message.msg_iov = &buffer;
        message.msg_iovlen = 1;
        ssize_t received = -1;
        do {
            received = recvmsg(descriptor_.get(), &message, 0);
        } while (received < 0 && errno == EINTR);
        if (received > 0 && (message.msg_flags & MSG_TRUNC) == 0) {
            unread_.assign(packet.begin(), packet.begin() + received);
            unreadOffset_ = 0;
            return true;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            waitFor(descriptor_.get(), POLLIN)) {
            continue;
        }
        close();
        return false;
    }
}

std::size_t GfxstreamPacketStream::readSome(void* bytes, std::size_t count) {
    if (!valid() || (!bytes && count != 0)) return 0;
    if (count == 0) return 0;
    if (unreadOffset_ == unread_.size() && !refill()) return 0;
    const std::size_t available = unread_.size() - unreadOffset_;
    const std::size_t copied = std::min(available, count);
    std::memcpy(bytes, unread_.data() + unreadOffset_, copied);
    unreadOffset_ += copied;
    if (unreadOffset_ == unread_.size()) {
        unread_.clear();
        unreadOffset_ = 0;
    }
    return copied;
}

bool GfxstreamPacketStream::readFully(void* bytes, std::size_t count) {
    if (!bytes && count != 0) return false;
    auto* destination = static_cast<std::uint8_t*>(bytes);
    while (count != 0) {
        const std::size_t received = readSome(destination, count);
        if (received == 0) return false;
        destination += received;
        count -= received;
    }
    return true;
}

} // namespace lcl::gpu
