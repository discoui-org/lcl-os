#include "lcl-gpu/gpu_client.hpp"

#include "system/ipc/gpu_protocol.hpp"

#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lcl::gpu {
namespace {

bool makeNonBlocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

GpuClient::~GpuClient() {
    disconnect();
}

GpuClient::GpuClient(GpuClient&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      handshakeRequested_(std::exchange(other.handshakeRequested_, false)),
      handshakeComplete_(std::exchange(other.handshakeComplete_, false)) {}

GpuClient& GpuClient::operator=(GpuClient&& other) noexcept {
    if (this != &other) {
        disconnect();
        fd_ = std::exchange(other.fd_, -1);
        handshakeRequested_ = std::exchange(other.handshakeRequested_, false);
        handshakeComplete_ = std::exchange(other.handshakeComplete_, false);
    }
    return *this;
}

bool GpuClient::connect(const std::string& socketPath) {
    disconnect();
    if (socketPath.empty() || socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
        return false;
    }
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        !makeNonBlocking(descriptor)) {
        close(descriptor);
        return false;
    }
    fd_ = descriptor;
    return true;
}

void GpuClient::disconnect() noexcept {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    handshakeRequested_ = false;
    handshakeComplete_ = false;
}

bool GpuClient::connected() const noexcept {
    return fd_ >= 0;
}

int GpuClient::fd() const noexcept {
    return fd_;
}

bool GpuClient::beginHandshake(uint64_t nonce) {
    if (fd_ < 0 || handshakeRequested_) return false;
    lcl::gpu_protocol::Hello hello{};
    hello.nonce = nonce;
    if (!lcl::gpu_protocol::sendPacket(fd_, lcl::gpu_protocol::Opcode::Hello,
                                       &hello, sizeof(hello))) {
        return false;
    }
    handshakeRequested_ = true;
    return true;
}

HandshakeStatus GpuClient::dispatch(DeviceCapabilities& capabilities) {
    if (fd_ < 0 || !handshakeRequested_) return HandshakeStatus::IoError;

    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    switch (lcl::gpu_protocol::receivePacket(fd_, header, payload)) {
        case lcl::gpu_protocol::ReceiveStatus::WouldBlock:
            return HandshakeStatus::Pending;
        case lcl::gpu_protocol::ReceiveStatus::Closed:
            disconnect();
            return HandshakeStatus::Closed;
        case lcl::gpu_protocol::ReceiveStatus::Invalid:
            disconnect();
            return HandshakeStatus::ProtocolError;
        case lcl::gpu_protocol::ReceiveStatus::Error:
            disconnect();
            return HandshakeStatus::IoError;
        case lcl::gpu_protocol::ReceiveStatus::Received:
            break;
    }

    const auto* device = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::DeviceInfo>(
        header, payload, lcl::gpu_protocol::Opcode::DeviceInfo);
    if (!device || device->maxImageDimension2D == 0) {
        disconnect();
        return HandshakeStatus::ProtocolError;
    }
    capabilities.vulkanApiVersion = device->vulkanApiVersion;
    capabilities.vendorId = device->vendorId;
    capabilities.deviceId = device->deviceId;
    capabilities.maxImageDimension2D = device->maxImageDimension2D;
    capabilities.supportsAndroidHardwareBuffer =
        (device->flags & lcl::gpu_protocol::kDeviceSupportsAndroidHardwareBuffer) != 0;
    capabilities.supportsExternalFenceFd =
        (device->flags & lcl::gpu_protocol::kDeviceSupportsExternalFenceFd) != 0;
    capabilities.deviceName = device->deviceName;
    handshakeRequested_ = false;
    handshakeComplete_ = true;
    return HandshakeStatus::Ready;
}

std::optional<GfxstreamPacketStream> GpuClient::openGfxstreamStream() {
    if (fd_ < 0 || handshakeRequested_ || !handshakeComplete_) return std::nullopt;
    const lcl::gpu_protocol::OpenGfxstreamStream open{};
    if (!lcl::gpu_protocol::sendPacket(fd_, lcl::gpu_protocol::Opcode::OpenGfxstreamStream,
                                       &open, sizeof(open))) {
        return std::nullopt;
    }
    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    client::OwnedFd descriptor;
    if (!receiveExpected(lcl::gpu_protocol::Opcode::GfxstreamStreamReady, 3'000,
                         header, payload, descriptor) || descriptor) {
        return std::nullopt;
    }
    const auto* ready = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::GfxstreamStreamReady>(
        header, payload, lcl::gpu_protocol::Opcode::GfxstreamStreamReady);
    if (!ready || ready->transportVersion != lcl::gpu_protocol::kGfxstreamTransportVersion ||
        ready->reserved != 0) {
        return std::nullopt;
    }
    handshakeComplete_ = false;
    return GfxstreamPacketStream(client::OwnedFd(std::exchange(fd_, -1)));
}

bool GpuClient::createGfxstreamColorBuffer(const uint32_t width, const uint32_t height,
                                           const uint32_t format,
                                           GfxstreamColorBuffer& result) {
    result = {};
    if (fd_ < 0 || handshakeRequested_ || !handshakeComplete_ || width == 0 || height == 0 ||
        format != lcl::gpu_protocol::kAndroidHardwareBufferRgba8888) {
        return false;
    }
    const lcl::gpu_protocol::CreateGfxstreamColorBuffer request{
        .width = width,
        .height = height,
        .format = format,
        .flags = 0,
    };
    if (!lcl::gpu_protocol::sendPacket(
            fd_, lcl::gpu_protocol::Opcode::CreateGfxstreamColorBuffer,
            &request, sizeof(request))) {
        return false;
    }
    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    client::OwnedFd descriptor;
    if (!receiveExpected(lcl::gpu_protocol::Opcode::GfxstreamColorBufferReady, 3'000,
                         header, payload, descriptor) || descriptor) {
        return false;
    }
    const auto* ready = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::GfxstreamColorBufferReady>(
        header, payload, lcl::gpu_protocol::Opcode::GfxstreamColorBufferReady);
    if (!ready || ready->targetId == 0 || ready->colorBufferHandle == 0 ||
        ready->width != width || ready->height != height || ready->format != format ||
        ready->stride < width) {
        return false;
    }
    result.targetId = ready->targetId;
    result.colorBufferHandle = ready->colorBufferHandle;
    result.width = ready->width;
    result.height = ready->height;
    result.format = ready->format;
    result.stride = ready->stride;
    return true;
}

bool GpuClient::destroyGfxstreamColorBuffer(const uint64_t targetId) {
    if (fd_ < 0 || handshakeRequested_ || !handshakeComplete_ || targetId == 0) return false;
    const lcl::gpu_protocol::DestroyGfxstreamColorBuffer request{targetId};
    return lcl::gpu_protocol::sendPacket(
        fd_, lcl::gpu_protocol::Opcode::DestroyGfxstreamColorBuffer,
        &request, sizeof(request));
}

bool GpuClient::receiveExpected(lcl::gpu_protocol::Opcode expected,
                                uint32_t timeoutMs,
                                lcl::gpu_protocol::Header& header,
                                std::vector<uint8_t>& payload,
                                client::OwnedFd& descriptor) {
    descriptor.reset();
    if (fd_ < 0) return false;
    pollfd ready{fd_, POLLIN, 0};
    int pollResult = -1;
    do {
        pollResult = poll(&ready, 1, static_cast<int>(timeoutMs));
    } while (pollResult < 0 && errno == EINTR);
    if (pollResult != 1 || (ready.revents & POLLIN) == 0) return false;
    int received = -1;
    if (lcl::gpu_protocol::receivePacketWithFd(fd_, header, payload, received) !=
            lcl::gpu_protocol::ReceiveStatus::Received ||
        header.opcode != expected) {
        if (received >= 0) close(received);
        return false;
    }
    descriptor = client::OwnedFd(received);
    return true;
}

bool GpuClient::clearColor(uint64_t requestId, uint32_t width, uint32_t height,
                           uint32_t format, uint32_t usage, float red, float green,
                           float blue, float alpha, ClearedNativeBuffer& result) {
    result = {};
    lcl::gpu_protocol::ClearColor request{};
    request.requestId = requestId;
    request.width = width;
    request.height = height;
    request.format = format;
    request.usage = usage;
    request.red = red;
    request.green = green;
    request.blue = blue;
    request.alpha = alpha;
    if (fd_ < 0 || !lcl::gpu_protocol::sendPacket(
            fd_, lcl::gpu_protocol::Opcode::ClearColor, &request, sizeof(request))) {
        return false;
    }
    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    client::OwnedFd fence;
    if (!receiveExpected(lcl::gpu_protocol::Opcode::ClearColorReady, 3'000,
                         header, payload, fence)) return false;
    const auto* ready = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::ClearColorReady>(
        header, payload, lcl::gpu_protocol::Opcode::ClearColorReady);
    if (!ready || ready->requestId != requestId || ready->bufferId == 0 ||
        ready->contentRevision != 1 || ready->width != width || ready->height != height ||
        ready->format != format || ready->stride < width * sizeof(uint32_t)) return false;
    result.bufferId = ready->bufferId;
    result.contentRevision = ready->contentRevision;
    result.width = ready->width;
    result.height = ready->height;
    result.stride = ready->stride;
    result.format = ready->format;
    result.acquireFence = std::move(fence);
    return true;
}

bool GpuClient::deliverNativeBuffer(uint64_t bufferId, int sidebandFd) {
    if (fd_ < 0 || bufferId == 0 || sidebandFd < 0) return false;
    lcl::gpu_protocol::DeliverNativeBuffer delivery{bufferId};
    if (!lcl::gpu_protocol::sendPacketWithFd(
            fd_, lcl::gpu_protocol::Opcode::DeliverNativeBuffer,
            &delivery, sizeof(delivery), sidebandFd)) return false;
    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    client::OwnedFd ignored;
    if (!receiveExpected(lcl::gpu_protocol::Opcode::DeliveryComplete, 3'000,
                         header, payload, ignored) || ignored) return false;
    const auto* completed = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::DeliveryComplete>(
        header, payload, lcl::gpu_protocol::Opcode::DeliveryComplete);
    return completed && completed->bufferId == bufferId && completed->status == 0 &&
           completed->reserved == 0;
}

bool GpuClient::releasePresentedBuffer(uint64_t bufferId,
                                       client::OwnedFd releaseFence) {
    if (fd_ < 0 || bufferId == 0) return false;
    lcl::gpu_protocol::ReleasePresentedBuffer release{bufferId};
    const int descriptor = releaseFence.get();
    return lcl::gpu_protocol::sendPacketWithFd(
        fd_, lcl::gpu_protocol::Opcode::ReleasePresentedBuffer,
        &release, sizeof(release), descriptor);
}

} // namespace lcl::gpu
