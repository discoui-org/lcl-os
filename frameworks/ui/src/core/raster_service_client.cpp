#include "raster_service_client.hpp"

#include <chrono>
#include <limits>
#include <utility>

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
    m_connection.configure(std::move(socketPath), grant);
}

void RasterServiceClient::disconnect() noexcept {
    m_connection.disconnect();
}

bool RasterServiceClient::isConfigured() const noexcept {
    return m_connection.isConfigured();
}

bool RasterServiceClient::connectIfNeeded() {
    return m_connection.prepare();
}

bool RasterServiceClient::uploadImage(
        const graphics::ImageResourceView& resource) {
    if (!connectIfNeeded() || resource.id == 0 || resource.contentRevision == 0 ||
        resource.width <= 0 || resource.height <= 0 ||
        resource.stridePixels < resource.width || !resource.pixels) return false;
    const size_t count = static_cast<size_t>(resource.stridePixels) *
        static_cast<size_t>(resource.height);
    return m_connection.uploadImage({
        resource.id, resource.contentRevision,
        static_cast<uint32_t>(resource.width),
        static_cast<uint32_t>(resource.height),
        static_cast<uint32_t>(resource.stridePixels), resource.opaque,
        std::span<const uint32_t>(resource.pixels, count)});
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
    return m_connection.uploadExternalBuffer(
        upload, frame.bufferFd, frame.acquireFenceFd);
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
    raster_protocol::CommitTransaction transaction{};
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
    return m_connection.commitRetained(
        transaction, mutations, displayList);
}

bool RasterServiceClient::submitPresentationAnimation(
        const raster_protocol::PresentationAnimation& animation) {
    const auto& grant = m_connection.surfaceGrant();
    if (!connectIfNeeded() || animation.grant.surfaceId != grant.surfaceId ||
        animation.grant.ownerPid != grant.ownerPid ||
        animation.grant.flags != grant.flags ||
        animation.grant.tokenHigh != grant.tokenHigh ||
        animation.grant.tokenLow != grant.tokenLow ||
        animation.transactionId == 0 || animation.nodeId == 0 ||
        animation.propertyMask == 0) {
        return false;
    }
    return m_connection.submitPresentationAnimation(animation);
}

std::vector<RasterServiceEvent> RasterServiceClient::pollEvents() {
    std::vector<RasterServiceEvent> result;
    for (auto& raw : m_connection.dispatch()) {
        if (const auto* discarded = std::get_if<
                raster_protocol::FrameDiscarded>(&raw)) {
            RasterServiceEvent event{};
            event.kind = RasterServiceEvent::Kind::FrameDiscarded;
            event.discarded = *discarded;
            result.push_back(event);
        } else if (auto* released = std::get_if<
                       lcl::client::RasterBufferReleased>(&raw)) {
            RasterServiceEvent event{};
            event.kind = RasterServiceEvent::Kind::ExternalBufferReleased;
            event.externalBufferRelease = {
                released->message, released->releaseFence.release()};
            result.push_back(event);
        } else if (const auto* animation = std::get_if<
                       raster_protocol::PresentationAnimationResult>(&raw)) {
            RasterServiceEvent event{};
            event.kind = RasterServiceEvent::Kind::PresentationAnimationResult;
            event.animation = *animation;
            result.push_back(event);
        }
    }
    return result;
}

} // namespace lcl::ui
