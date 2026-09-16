#include "lcl-gfxstream/guest_socket_iostream.hpp"

#include <algorithm>
#include <cerrno>
#include <poll.h>

namespace lcl::gfxstream {

std::unique_ptr<GuestSocketIOStream> GuestSocketIOStream::openAuthorized(
    const std::string& socketPath, std::uint64_t nonce, gpu::DeviceCapabilities* capabilities) {
    gpu::GpuClient client;
    if (!client.connect(socketPath) || !client.beginHandshake(nonce)) return nullptr;
    gpu::DeviceCapabilities discovered{};
    for (;;) {
        pollfd ready{client.fd(), POLLIN, 0};
        int result = -1;
        do {
            result = poll(&ready, 1, 3'000);
        } while (result < 0 && errno == EINTR);
        if (result != 1 || (ready.revents & POLLIN) == 0) return nullptr;
        const auto status = client.dispatch(discovered);
        if (status == gpu::HandshakeStatus::Ready) break;
        if (status != gpu::HandshakeStatus::Pending) return nullptr;
    }
    auto stream = client.openGfxstreamStream();
    if (!stream) return nullptr;
    if (capabilities) *capabilities = std::move(discovered);
    return std::make_unique<GuestSocketIOStream>(std::move(*stream));
}

GuestSocketIOStream::GuestSocketIOStream(gpu::GfxstreamPacketStream stream,
                                         std::size_t bufferSize)
    : ::gfxstream::guest::IOStream(bufferSize), stream_(std::move(stream)) {}

GuestSocketIOStream::~GuestSocketIOStream() {
    (void)flush();
}

bool GuestSocketIOStream::valid() const noexcept {
    return stream_.valid();
}

void* GuestSocketIOStream::allocBuffer(std::size_t minimumSize) {
    buffer_.resize(minimumSize);
    return buffer_.data();
}

int GuestSocketIOStream::commitBuffer(std::size_t size) {
    return size <= buffer_.size() && stream_.writeFully(buffer_.data(), size)
               ? static_cast<int>(size)
               : -1;
}

const unsigned char* GuestSocketIOStream::readFully(void* buffer, std::size_t size) {
    return stream_.readFully(buffer, size) ? static_cast<const unsigned char*>(buffer) : nullptr;
}

const unsigned char* GuestSocketIOStream::commitBufferAndReadFully(std::size_t writeSize,
                                                                     void* readBuffer,
                                                                     std::size_t readSize) {
    if (writeSize > buffer_.size() || !stream_.writeFully(buffer_.data(), writeSize)) {
        return nullptr;
    }
    return readFully(readBuffer, readSize);
}

const unsigned char* GuestSocketIOStream::read(void* buffer, std::size_t* inOutSize) {
    if (!buffer || !inOutSize) return nullptr;
    const std::size_t received = stream_.readSome(buffer, *inOutSize);
    *inOutSize = received;
    return received != 0 ? static_cast<const unsigned char*>(buffer) : nullptr;
}

int GuestSocketIOStream::writeFully(const void* buffer, std::size_t size) {
    return stream_.writeFully(buffer, size) ? 0 : -1;
}

} // namespace lcl::gfxstream
