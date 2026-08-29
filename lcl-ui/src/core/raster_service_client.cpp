#include "raster_service_client.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace lcl::ui {

RasterServiceClient::~RasterServiceClient() {
    disconnect();
}

void RasterServiceClient::configure(
        std::string socketPath, const raster_protocol::SurfaceGrant& grant) {
    const bool changed = socketPath != m_socketPath ||
        grant.tokenHigh != m_grant.tokenHigh || grant.tokenLow != m_grant.tokenLow;
    if (changed) disconnect();
    m_socketPath = std::move(socketPath);
    m_grant = grant;
}

void RasterServiceClient::disconnect() noexcept {
    if (m_fd >= 0) close(m_fd);
    m_fd = -1;
}

bool RasterServiceClient::isConfigured() const noexcept {
    return !m_socketPath.empty() && m_grant.surfaceId != 0 &&
        (m_grant.tokenHigh != 0 || m_grant.tokenLow != 0);
}

bool RasterServiceClient::connectIfNeeded() {
    if (m_fd >= 0) return true;
    if (!isConfigured()) return false;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    std::strncpy(address.sun_path, m_socketPath.c_str(),
                 sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    m_fd = fd;
    ++m_connectionGeneration;
    if (m_connectionGeneration == 0) m_connectionGeneration = 1;
    return true;
}

int RasterServiceClient::createSealedMemfd(
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

bool RasterServiceClient::uploadImage(
        const graphics::ImageResourceView& resource) {
    if (!connectIfNeeded() || resource.id == 0 || resource.contentRevision == 0 ||
        resource.width <= 0 || resource.height <= 0 ||
        resource.stridePixels < resource.width || !resource.pixels) return false;
    const size_t bytes = static_cast<size_t>(resource.stridePixels) *
        static_cast<size_t>(resource.height) * sizeof(uint32_t);
    const int memfd = createSealedMemfd(
        "lcl-raster-image", resource.pixels, bytes);
    if (memfd < 0) return false;
    raster_protocol::UploadImage upload{};
    upload.grant = m_grant;
    upload.resourceId = resource.id;
    upload.contentRevision = resource.contentRevision;
    upload.width = static_cast<uint32_t>(resource.width);
    upload.height = static_cast<uint32_t>(resource.height);
    upload.stridePixels = static_cast<uint32_t>(resource.stridePixels);
    upload.opaque = resource.opaque ? 1u : 0u;
    upload.byteSize = bytes;
    const bool sent = raster_protocol::sendPacket(
        m_fd, raster_protocol::Opcode::UploadImage, upload, memfd);
    close(memfd);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

bool RasterServiceClient::submitFrame(
        uint64_t configureSerial, uint64_t frameSerial,
        uint64_t baseFrameSerial, uint64_t geometryGeneration,
        float logicalWidth,
        float logicalHeight, float bufferScale,
        const graphics::RectF& damage, bool replacesScene,
        const std::vector<uint8_t>& displayList) {
    if (!connectIfNeeded() || configureSerial == 0 || frameSerial == 0 ||
        displayList.empty() || damage.isEmpty()) return false;
    const int memfd = createSealedMemfd(
        "lcl-raster-frame", displayList.data(), displayList.size());
    if (memfd < 0) return false;
    raster_protocol::SubmitFrame submit{};
    submit.grant = m_grant;
    submit.configureSerial = configureSerial;
    submit.frameSerial = frameSerial;
    submit.baseFrameSerial = replacesScene ? 0 : baseFrameSerial;
    submit.geometryGeneration = geometryGeneration;
    submit.logicalWidth = logicalWidth;
    submit.logicalHeight = logicalHeight;
    submit.bufferScale = bufferScale;
    submit.damageX = damage.x;
    submit.damageY = damage.y;
    submit.damageWidth = damage.width;
    submit.damageHeight = damage.height;
    submit.displayListSize = static_cast<uint32_t>(displayList.size());
    submit.flags = replacesScene
        ? raster_protocol::kSubmitReplacesScene : 0u;
    const bool sent = raster_protocol::sendPacket(
        m_fd, raster_protocol::Opcode::SubmitFrame, submit, memfd);
    close(memfd);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
    return sent;
}

std::vector<raster_protocol::FrameDiscarded>
RasterServiceClient::pollDiscards() {
    std::vector<raster_protocol::FrameDiscarded> result;
    if (m_fd < 0) return result;
    while (true) {
        raster_protocol::Header header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto status = raster_protocol::receivePacket(
            m_fd, header, payload, receivedFd);
        if (receivedFd >= 0) close(receivedFd);
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
            result.push_back(*discarded);
        }
    }
    return result;
}

} // namespace lcl::ui
