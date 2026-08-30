#include "raster_service_client.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
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
namespace {

uint64_t monotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

raster_protocol::RetainedNodeState toProtocolNode(
        const detail::RetainedRenderNode& node) {
    raster_protocol::RetainedNodeState result{};
    result.id = node.id;
    result.parentId = node.parentId;
    result.contentRevision = node.contentRevision;
    result.propertyRevision = node.propertyRevision;
    result.boundaryReasons = static_cast<uint32_t>(node.boundaryReasons);
    result.siblingIndex = node.siblingIndex;
    if (node.externalBufferId != 0 && node.externalBufferRevision != 0) {
        result.flags |= raster_protocol::kNodeHasExternalBuffer;
        result.externalBufferId = node.externalBufferId;
        result.externalBufferRevision = node.externalBufferRevision;
    }
    result.layoutX = node.layoutBounds.x;
    result.layoutY = node.layoutBounds.y;
    result.layoutWidth = node.layoutBounds.width;
    result.layoutHeight = node.layoutBounds.height;
    result.presentationX = node.presentationBounds.x;
    result.presentationY = node.presentationBounds.y;
    result.presentationWidth = node.presentationBounds.width;
    result.presentationHeight = node.presentationBounds.height;
    if (node.clipBounds) {
        result.flags |= raster_protocol::kNodeHasClip;
        result.clipX = node.clipBounds->x;
        result.clipY = node.clipBounds->y;
        result.clipWidth = node.clipBounds->width;
        result.clipHeight = node.clipBounds->height;
    }
    result.opacity = node.presentation.opacity;
    result.translationX = node.presentation.translationX;
    result.translationY = node.presentation.translationY;
    result.scaleX = node.presentation.scaleX;
    result.scaleY = node.presentation.scaleY;
    result.rotationRadians = node.presentation.rotationRadians;
    result.originX = node.presentation.originX;
    result.originY = node.presentation.originY;
    return result;
}

std::vector<raster_protocol::NodeMutation> toProtocolMutations(
        const detail::RenderTreeTransaction& transaction) {
    std::vector<raster_protocol::NodeMutation> mutations;
    mutations.reserve(transaction.creates.size() + transaction.removals.size() +
                      transaction.updates.size() * 2u);
    for (const auto& node : transaction.creates) {
        mutations.push_back({
            raster_protocol::NodeMutationType::CreateNode, 0,
            toProtocolNode(node)});
    }
    for (const auto& update : transaction.updates) {
        if (update.contentChanged) {
            mutations.push_back({
                raster_protocol::NodeMutationType::UpdateContent, 0,
                toProtocolNode(update.node)});
        }
        if (update.propertiesChanged) {
            mutations.push_back({
                raster_protocol::NodeMutationType::SetProperties, 0,
                toProtocolNode(update.node)});
        }
    }
    for (uint64_t id : transaction.removals) {
        raster_protocol::RetainedNodeState node{};
        node.id = id;
        mutations.push_back({
            raster_protocol::NodeMutationType::RemoveNode, 0, node});
    }
    return mutations;
}

} // namespace

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
    for (auto& release : m_externalBufferReleases) {
        if (release.releaseFenceFd >= 0) close(release.releaseFenceFd);
    }
    m_externalBufferReleases.clear();
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

bool RasterServiceClient::uploadExternalBuffer(
        const ExternalBufferFrame& frame) {
    constexpr uint32_t kMaxDimension = 16384;
    if (!connectIfNeeded() || frame.bufferId == 0 ||
        frame.contentRevision == 0 || frame.width == 0 || frame.height == 0 ||
        frame.width > kMaxDimension || frame.height > kMaxDimension ||
        frame.stride < frame.width * sizeof(uint32_t) ||
        frame.stride % sizeof(uint32_t) != 0 ||
        frame.stride > kMaxDimension * sizeof(uint32_t) ||
        frame.format != kExternalBufferFormatArgb8888 ||
        frame.bufferFd < 0) {
        return false;
    }
    raster_protocol::UploadExternalBuffer upload{};
    upload.grant = m_grant;
    upload.bufferId = frame.bufferId;
    upload.contentRevision = frame.contentRevision;
    upload.transport = frame.transport ==
            ExternalBufferTransport::ImmutableShmArgb8888
        ? raster_protocol::ExternalBufferTransport::ImmutableShmArgb8888
        : raster_protocol::ExternalBufferTransport::DmaBufArgb8888;
    upload.width = frame.width;
    upload.height = frame.height;
    upload.stride = frame.stride;
    upload.format = frame.format;
    upload.modifier = frame.modifier;
    upload.byteSize = frame.byteSize;
    if (!raster_protocol::sendPacket(
            m_fd, raster_protocol::Opcode::UploadExternalBuffer,
            upload, frame.bufferFd)) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
        return false;
    }
    if (frame.acquireFenceFd < 0) return true;

    raster_protocol::SetExternalBufferFence fence{};
    fence.grant = m_grant;
    fence.bufferId = frame.bufferId;
    fence.contentRevision = frame.contentRevision;
    if (!raster_protocol::sendPacket(
            m_fd, raster_protocol::Opcode::SetExternalBufferFence,
            fence, frame.acquireFenceFd)) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) disconnect();
        return false;
    }
    return true;
}

bool RasterServiceClient::commitTransaction(
        uint64_t configureSerial, uint64_t frameSerial,
        uint64_t baseFrameSerial, uint64_t geometryGeneration,
        float logicalWidth,
        float logicalHeight, float bufferScale,
        const graphics::RectF& damage,
        const detail::RenderTreeTransaction& renderTreeTransaction,
        const std::vector<uint8_t>& displayList,
        uint64_t clientFrameStartNs) {
    if (!connectIfNeeded() || configureSerial == 0 || frameSerial == 0 ||
        damage.isEmpty()) return false;
    const auto mutations = toProtocolMutations(renderTreeTransaction);
    if (mutations.size() > std::numeric_limits<uint32_t>::max()) return false;
    const int memfd = displayList.empty()
        ? -1
        : createSealedMemfd(
              "lcl-raster-frame", displayList.data(), displayList.size());
    if (!displayList.empty() && memfd < 0) return false;
    raster_protocol::CommitTransaction transaction{};
    transaction.grant = m_grant;
    transaction.configureSerial = configureSerial;
    transaction.frameSerial = frameSerial;
    transaction.baseFrameSerial = renderTreeTransaction.replacesTree
        ? 0 : baseFrameSerial;
    transaction.geometryGeneration = geometryGeneration;
    transaction.logicalWidth = logicalWidth;
    transaction.logicalHeight = logicalHeight;
    transaction.bufferScale = bufferScale;
    transaction.damageX = damage.x;
    transaction.damageY = damage.y;
    transaction.damageWidth = damage.width;
    transaction.damageHeight = damage.height;
    transaction.displayListSize = static_cast<uint32_t>(displayList.size());
    transaction.mutationCount = static_cast<uint32_t>(mutations.size());
    transaction.flags = renderTreeTransaction.replacesTree
        ? raster_protocol::kTransactionReplacesTree : 0u;
    if (clientFrameStartNs != 0) {
        transaction.clientFrameStartNs = clientFrameStartNs;
        transaction.clientSubmitNs = monotonicNowNs();
    }
    const bool sent = raster_protocol::sendCommitTransaction(
        m_fd, transaction, mutations, memfd);
    if (memfd >= 0) close(memfd);
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
        if (status == raster_protocol::ReceiveStatus::WouldBlock) break;
        if (status == raster_protocol::ReceiveStatus::Closed ||
            status == raster_protocol::ReceiveStatus::Error) {
            disconnect();
            if (receivedFd >= 0) close(receivedFd);
            break;
        }
        if (status != raster_protocol::ReceiveStatus::Received) continue;
        if (const auto* discarded = raster_protocol::payloadAs<
                raster_protocol::FrameDiscarded>(
                header, payload, raster_protocol::Opcode::FrameDiscarded)) {
            result.push_back(*discarded);
        } else if (const auto* released = raster_protocol::payloadAs<
                       raster_protocol::ExternalBufferReleased>(
                       header, payload,
                       raster_protocol::Opcode::ExternalBufferReleased)) {
            m_externalBufferReleases.push_back({*released, receivedFd});
            receivedFd = -1;
        }
        if (receivedFd >= 0) close(receivedFd);
    }
    return result;
}

std::vector<ExternalBufferRelease>
RasterServiceClient::takeExternalBufferReleases() {
    return std::exchange(m_externalBufferReleases, {});
}

} // namespace lcl::ui
