#include "lcl-client/raster_connection.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace lcl::client {

RasterConnection::~RasterConnection() { disconnect(); }

void RasterConnection::configure(
        std::string socketPath,
        const raster_protocol::SurfaceGrant& grant) {
    const bool changed = socketPath != m_socketPath ||
        grant.tokenHigh != m_grant.tokenHigh ||
        grant.tokenLow != m_grant.tokenLow;
    if (changed) disconnect();
    m_socketPath = std::move(socketPath);
    m_grant = grant;
}

void RasterConnection::disconnect() noexcept {
    if (m_fd >= 0) close(m_fd);
    m_fd = -1;
}

bool RasterConnection::isConfigured() const noexcept {
    return !m_socketPath.empty() && m_grant.surfaceId != 0 &&
        (m_grant.tokenHigh != 0 || m_grant.tokenLow != 0);
}

bool RasterConnection::connectIfNeeded() {
    if (m_fd >= 0) return true;
    if (!isConfigured()) return false;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    std::memcpy(address.sun_path, m_socketPath.c_str(),
                m_socketPath.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }
    const int descriptorFlags = fcntl(fd, F_GETFD, 0);
    if (descriptorFlags >= 0) {
        (void)fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC);
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return false;
    }
    m_fd = fd;
    ++m_generation;
    if (m_generation == 0) m_generation = 1;
    return true;
}

bool RasterConnection::prepare() { return connectIfNeeded(); }

int RasterConnection::createSealedMemfd(
        const char* name, const void* data, size_t bytes) {
    if (!data || bytes == 0) return -1;
#if defined(SYS_memfd_create)
    const int fd = static_cast<int>(syscall(
        SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return -1;
    }
    std::memcpy(mapping, data, bytes);
    munmap(mapping, bytes);
#if defined(F_ADD_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (fcntl(fd, F_ADD_SEALS, seals) != 0) {
        close(fd);
        return -1;
    }
#endif
    return fd;
#else
    (void)name;
    return -1;
#endif
}

bool RasterConnection::uploadImage(const LegacyImageUpload& image) {
    if (!connectIfNeeded() || image.resourceId == 0 ||
        image.contentRevision == 0 || image.width == 0 || image.height == 0 ||
        image.stridePixels < image.width || image.pixels.empty()) return false;
    const size_t count = static_cast<size_t>(image.stridePixels) * image.height;
    if (image.pixels.size() < count) return false;
    const size_t bytes = count * sizeof(uint32_t);
    OwnedFd memfd(createSealedMemfd("lcl-raster-image",
                                    image.pixels.data(), bytes));
    if (!memfd) return false;
    raster_protocol::UploadImage upload{};
    upload.grant = m_grant;
    upload.resourceId = image.resourceId;
    upload.contentRevision = image.contentRevision;
    upload.width = image.width;
    upload.height = image.height;
    upload.stridePixels = image.stridePixels;
    upload.opaque = image.opaque ? 1u : 0u;
    upload.byteSize = bytes;
    const bool sent = raster_protocol::sendPacket(
        m_fd, raster_protocol::Opcode::UploadImage, upload, memfd.get());
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

bool RasterConnection::uploadExternalBuffer(
        raster_protocol::UploadExternalBuffer upload, int bufferFd,
        int acquireFenceFd) {
    if (!connectIfNeeded() || upload.bufferId == 0 ||
        upload.contentRevision == 0 || bufferFd < 0) return false;
    upload.grant = m_grant;
    if (!raster_protocol::sendPacket(
            m_fd, raster_protocol::Opcode::UploadExternalBuffer,
            upload, bufferFd)) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
        return false;
    }
    if (acquireFenceFd < 0) return true;
    raster_protocol::SetExternalBufferFence fence{};
    fence.grant = m_grant;
    fence.bufferId = upload.bufferId;
    fence.contentRevision = upload.contentRevision;
    const bool sent = raster_protocol::sendPacket(
        m_fd, raster_protocol::Opcode::SetExternalBufferFence,
        fence, acquireFenceFd);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

bool RasterConnection::commitRetained(
        raster_protocol::CommitTransaction transaction,
        std::span<const raster_protocol::NodeMutation> mutations,
        std::span<const uint8_t> displayList) {
    if (!connectIfNeeded() || transaction.configureSerial == 0 ||
        transaction.frameSerial == 0 ||
        mutations.size() > std::numeric_limits<uint32_t>::max() ||
        displayList.size() > std::numeric_limits<uint32_t>::max()) return false;
    transaction.grant = m_grant;
    transaction.mutationCount = static_cast<uint32_t>(mutations.size());
    transaction.displayListSize = static_cast<uint32_t>(displayList.size());
    OwnedFd memfd(displayList.empty() ? -1 : createSealedMemfd(
        "lcl-raster-frame", displayList.data(), displayList.size()));
    if (!displayList.empty() && !memfd) return false;
    const bool sent = raster_protocol::sendCommitTransaction(
        m_fd, transaction, mutations, memfd.get());
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

bool RasterConnection::submitPresentationAnimation(
        raster_protocol::PresentationAnimation animation) {
    if (!connectIfNeeded()) return false;
    animation.grant = m_grant;
    const bool sent = raster_protocol::sendPresentationAnimation(m_fd, animation);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

std::vector<RasterEvent> RasterConnection::dispatch() {
    std::vector<RasterEvent> events;
    if (m_fd < 0) return events;
    while (true) {
        raster_protocol::Header header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto status = raster_protocol::receivePacket(
            m_fd, header, payload, receivedFd);
        OwnedFd received(receivedFd);
        if (status == raster_protocol::ReceiveStatus::WouldBlock) break;
        if (status == raster_protocol::ReceiveStatus::Closed ||
            status == raster_protocol::ReceiveStatus::Error) {
            disconnect();
            break;
        }
        if (status != raster_protocol::ReceiveStatus::Received) continue;
        if (const auto* discarded = raster_protocol::payloadAs<
                raster_protocol::FrameDiscarded>(
                header, payload, raster_protocol::Opcode::FrameDiscarded)) {
            events.emplace_back(*discarded);
        } else if (const auto* released = raster_protocol::payloadAs<
                       raster_protocol::ExternalBufferReleased>(
                       header, payload,
                       raster_protocol::Opcode::ExternalBufferReleased)) {
            events.emplace_back(RasterBufferReleased{
                *released, std::move(received)});
        } else if (const auto* animation = raster_protocol::payloadAs<
                       raster_protocol::PresentationAnimationResult>(
                       header, payload,
                       raster_protocol::Opcode::PresentationAnimationResult)) {
            events.emplace_back(*animation);
        }
    }
    return events;
}

} // namespace lcl::client

