#include "system/ipc/raster_protocol.hpp"
#include "lcl-graphics/display_list_wire.hpp"
#include "system/render/client_egl_context.hpp"
#include "system/render/raster_renderer.hpp"
#include "platforms/common/retained_output_damage.hpp"
#include "system/render/retained_scroll_tiles.hpp"
#include "platforms/common/native_buffer.hpp"
#if !defined(__ANDROID__)
#include "system/security/session_user.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace {

using lcl::raster_protocol::FrameDiscarded;
using lcl::raster_protocol::CommitTransaction;
using lcl::raster_protocol::LayerReady;
using lcl::raster_protocol::NodeMutation;
using lcl::raster_protocol::Opcode;
using lcl::raster_protocol::RetainedNodeState;
using lcl::raster_protocol::SurfaceGrant;

struct TokenKey {
    uint64_t high{0};
    uint64_t low{0};
    bool operator==(const TokenKey&) const = default;
};

uint64_t monotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct TokenHash {
    size_t operator()(const TokenKey& key) const noexcept {
        return std::hash<uint64_t>{}(key.high) ^
            (std::hash<uint64_t>{}(key.low) << 1u);
    }
};

TokenKey tokenOf(const SurfaceGrant& grant) {
    return {grant.tokenHigh, grant.tokenLow};
}

struct ImageKey {
    uint64_t id{0};
    uint64_t revision{0};
    bool operator==(const ImageKey&) const = default;
};

struct ImageHash {
    size_t operator()(const ImageKey& key) const noexcept {
        return std::hash<uint64_t>{}(key.id) ^
            (std::hash<uint64_t>{}(key.revision) << 1u);
    }
};

struct ImageResource {
    uint64_t rendererId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stridePixels{0};
    bool opaque{false};
    std::vector<uint32_t> pixels;
};

using ExternalKey = ImageKey;
using ExternalHash = ImageHash;

struct ExternalBufferResource {
    lcl::raster_protocol::ExternalBufferTransport transport{
        lcl::raster_protocol::ExternalBufferTransport::DmaBufArgb8888};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    int bufferFd{-1};
    int acquireFenceFd{-1};
    int clientFd{-1};
    std::vector<uint32_t> pixels{};

    ExternalBufferResource() = default;
    ExternalBufferResource(const ExternalBufferResource&) = delete;
    ExternalBufferResource& operator=(const ExternalBufferResource&) = delete;
    ExternalBufferResource(ExternalBufferResource&& other) noexcept {
        *this = std::move(other);
    }
    ExternalBufferResource& operator=(ExternalBufferResource&& other) noexcept {
        if (this == &other) return *this;
        release();
        transport = other.transport;
        width = other.width;
        height = other.height;
        stride = other.stride;
        format = other.format;
        modifier = other.modifier;
        bufferFd = other.bufferFd;
        acquireFenceFd = other.acquireFenceFd;
        clientFd = other.clientFd;
        pixels = std::move(other.pixels);
        other.bufferFd = -1;
        other.acquireFenceFd = -1;
        other.clientFd = -1;
        return *this;
    }
    ~ExternalBufferResource() { release(); }

    void release() noexcept {
        if (bufferFd >= 0) close(bufferFd);
        if (acquireFenceFd >= 0) close(acquireFenceFd);
        bufferFd = -1;
        acquireFenceFd = -1;
    }
};

struct LayerSlot {
    uint64_t layerId{0};
    int fd{-1};
    uint32_t* pixels{nullptr};
    size_t bytes{0};
    uint32_t width{0};
    uint32_t height{0};
    bool busy{false};

    LayerSlot() = default;
    LayerSlot(const LayerSlot&) = delete;
    LayerSlot& operator=(const LayerSlot&) = delete;
    LayerSlot(LayerSlot&& other) noexcept { *this = std::move(other); }
    LayerSlot& operator=(LayerSlot&& other) noexcept {
        if (this == &other) return *this;
        release();
        layerId = other.layerId;
        fd = other.fd;
        pixels = other.pixels;
        bytes = other.bytes;
        width = other.width;
        height = other.height;
        busy = other.busy;
        other.fd = -1;
        other.pixels = nullptr;
        other.bytes = 0;
        other.busy = false;
        return *this;
    }
    ~LayerSlot() { release(); }

    void release() noexcept {
        if (pixels && bytes) munmap(pixels, bytes);
        if (fd >= 0) close(fd);
        fd = -1;
        pixels = nullptr;
        bytes = 0;
        busy = false;
    }
};

struct SurfaceState {
    SurfaceGrant grant{};
    std::unordered_map<uint64_t, RetainedNodeState> retainedNodes;
    uint64_t retainedRootId{0};
    lcl::render::RetainedScrollTileCache scrollTiles;
    std::unordered_map<ImageKey, ImageResource, ImageHash> images;
    std::unordered_map<ExternalKey, ExternalBufferResource, ExternalHash>
        externalBuffers;
    std::unordered_map<uint64_t, uint64_t> cachedLayerNamespaces;
    std::array<LayerSlot, 3> slots;
    std::vector<uint32_t> softwarePixels;
    std::unique_ptr<lcl::render::RasterRenderer> softwareRenderer;
    uint64_t softwareFrameSerial{0};
    uint64_t softwareGeometryGeneration{0};
    uint32_t softwareWidth{0};
    uint32_t softwareHeight{0};
    float softwareScale{0.0f};
    std::unique_ptr<lcl::render::ClientEGLContext> gpuContext;
    std::unique_ptr<lcl::render::RasterRenderer> gpuRenderer;
    std::unordered_map<uint64_t, uint32_t> gpuLayers;
    std::unordered_map<uint32_t, uint64_t> gpuBufferIds;
    lcl::platform::RetainedOutputDamageTracker gpuOutputDamage;
    uint64_t gpuFrameSerial{0};
    uint64_t gpuGeometryGeneration{0};
    uint32_t gpuWidth{0};
    uint32_t gpuHeight{0};
    float gpuScale{0.0f};
    bool gpuUnavailable{false};
};

struct Client {
    int fd{-1};
    pid_t pid{0};
};

struct PendingFrame {
    int clientFd{-1};
    CommitTransaction transaction{};
    std::vector<NodeMutation> mutations;
    int displayListFd{-1};
};

int createMemfd(const char* name, size_t bytes) {
#if defined(SYS_memfd_create)
    const int fd = static_cast<int>(syscall(
        SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
#else
    (void)name;
    (void)bytes;
    return -1;
#endif
}

bool isImmutableMemfd(int fd, size_t expectedBytes) {
    if (fd < 0 || expectedBytes == 0) return false;
    struct stat info{};
    if (fstat(fd, &info) != 0 ||
        static_cast<uint64_t>(info.st_size) != expectedBytes) return false;
#if defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    const int seals = fcntl(fd, F_GET_SEALS);
    constexpr int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    return seals >= 0 && (seals & required) == required;
#else
    return true;
#endif
}

bool readExact(int fd, void* destination, size_t bytes) {
    if (fd < 0 || !destination || bytes == 0) return false;
    auto* cursor = static_cast<uint8_t*>(destination);
    size_t offset = 0;
    while (offset < bytes) {
        const ssize_t count = pread(
            fd, cursor + offset, bytes - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool replacesRetainedScene(const CommitTransaction& transaction) noexcept {
    return (transaction.flags &
            lcl::raster_protocol::kTransactionReplacesTree) != 0;
}

bool hasValidRetainedFrameMetadata(
        const CommitTransaction& submit) noexcept {
    constexpr float kTolerance = 0.001f;
    constexpr float kMaxPixelExtent = 16384.0f;
    const bool finite = std::isfinite(submit.logicalWidth) &&
        std::isfinite(submit.logicalHeight) &&
        std::isfinite(submit.bufferScale) &&
        std::isfinite(submit.damageX) && std::isfinite(submit.damageY) &&
        std::isfinite(submit.damageWidth) &&
        std::isfinite(submit.damageHeight);
    if (!finite || submit.logicalWidth <= 0.0f ||
        submit.logicalHeight <= 0.0f || submit.bufferScale < 0.5f ||
        submit.bufferScale > 4.0f || submit.damageX < 0.0f ||
        submit.logicalWidth * submit.bufferScale > kMaxPixelExtent ||
        submit.logicalHeight * submit.bufferScale > kMaxPixelExtent ||
        submit.damageY < 0.0f || submit.damageWidth <= 0.0f ||
        submit.damageHeight <= 0.0f ||
        submit.damageX + submit.damageWidth >
            submit.logicalWidth + kTolerance ||
        submit.damageY + submit.damageHeight >
            submit.logicalHeight + kTolerance ||
        (submit.flags &
         ~lcl::raster_protocol::kTransactionReplacesTree) != 0 ||
        submit.reserved != 0) {
        return false;
    }
    if (!replacesRetainedScene(submit)) return submit.baseFrameSerial != 0;
    return submit.baseFrameSerial == 0 &&
        submit.damageX <= kTolerance && submit.damageY <= kTolerance &&
        submit.damageX + submit.damageWidth >=
            submit.logicalWidth - kTolerance &&
        submit.damageY + submit.damageHeight >=
            submit.logicalHeight - kTolerance;
}

bool retainedBaseMatches(const CommitTransaction& submit, uint64_t frameSerial,
                         uint64_t geometryGeneration, uint32_t width,
                         uint32_t height, float scale) noexcept {
    if (replacesRetainedScene(submit)) return true;
    const uint32_t nextWidth = std::max(1u, static_cast<uint32_t>(
        std::ceil(submit.logicalWidth * submit.bufferScale)));
    const uint32_t nextHeight = std::max(1u, static_cast<uint32_t>(
        std::ceil(submit.logicalHeight * submit.bufferScale)));
    return frameSerial != 0 && submit.baseFrameSerial == frameSerial &&
        submit.geometryGeneration == geometryGeneration &&
        width == nextWidth && height == nextHeight &&
        std::fabs(scale - submit.bufferScale) <= 0.0001f;
}

struct PixelDamage {
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
};

PixelDamage pixelDamageFor(const CommitTransaction& submit, uint32_t width,
                           uint32_t height) noexcept {
    const uint32_t left = std::min(width, static_cast<uint32_t>(std::floor(
        submit.damageX * submit.bufferScale)));
    const uint32_t top = std::min(height, static_cast<uint32_t>(std::floor(
        submit.damageY * submit.bufferScale)));
    const uint32_t right = std::min(width, static_cast<uint32_t>(std::ceil(
        (submit.damageX + submit.damageWidth) * submit.bufferScale)));
    const uint32_t bottom = std::min(height, static_cast<uint32_t>(std::ceil(
        (submit.damageY + submit.damageHeight) * submit.bufferScale)));
    return {left, top, right > left ? right - left : 0,
            bottom > top ? bottom - top : 0};
}

bool validNodeState(const RetainedNodeState& node) noexcept {
    constexpr uint32_t kBoundaryMask = 0xffu;
    constexpr uint32_t kExternalReason = 1u << 7u;
    constexpr uint32_t kAllowedFlags =
        lcl::raster_protocol::kNodeHasClip |
        lcl::raster_protocol::kNodeHasExternalBuffer;
    const auto finite = [](float value) { return std::isfinite(value); };
    if (node.id == 0 || node.contentRevision == 0 ||
        node.boundaryReasons == 0 || node.reserved != 0 ||
        (node.boundaryReasons & ~kBoundaryMask) != 0 ||
        (node.flags & ~kAllowedFlags) != 0) {
        return false;
    }
    const bool externalReason =
        (node.boundaryReasons & kExternalReason) != 0;
    const bool externalBinding =
        (node.flags & lcl::raster_protocol::kNodeHasExternalBuffer) != 0;
    const bool completeExternalBinding =
        node.externalBufferId != 0 && node.externalBufferRevision != 0;
    const bool partialExternalBinding =
        (node.externalBufferId == 0) !=
        (node.externalBufferRevision == 0);
    if (partialExternalBinding ||
        externalBinding != completeExternalBinding ||
        (!externalReason &&
         (externalBinding || node.externalBufferId != 0 ||
          node.externalBufferRevision != 0))) {
        return false;
    }
    // An external node may deliberately have no binding while waiting for its
    // first producer frame. Every non-external node is forced empty above.
    const float values[]{
        node.layoutX, node.layoutY, node.layoutWidth, node.layoutHeight,
        node.presentationX, node.presentationY,
        node.presentationWidth, node.presentationHeight,
        node.clipX, node.clipY, node.clipWidth, node.clipHeight,
        node.opacity, node.translationX, node.translationY,
        node.scaleX, node.scaleY, node.rotationRadians,
        node.originX, node.originY,
    };
    if (!std::all_of(std::begin(values), std::end(values), finite) ||
        node.layoutWidth < 0.0f || node.layoutHeight < 0.0f ||
        node.presentationWidth < 0.0f || node.presentationHeight < 0.0f ||
        node.opacity < 0.0f || node.opacity > 1.0f ||
        node.scaleX < 0.0f || node.scaleY < 0.0f ||
        node.originX < 0.0f || node.originX > 1.0f ||
        node.originY < 0.0f || node.originY > 1.0f) {
        return false;
    }
    return (node.flags & lcl::raster_protocol::kNodeHasClip) == 0 ||
        (node.clipWidth >= 0.0f && node.clipHeight >= 0.0f);
}

bool applyNodeMutations(
        const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations,
        std::unordered_map<uint64_t, RetainedNodeState>& nodes,
        uint64_t& rootId) {
    constexpr size_t kMaxRetainedNodes = 4096;
    constexpr size_t kMaxMutations = kMaxRetainedNodes * 2;
    if (mutations.size() != transaction.mutationCount ||
        mutations.size() > kMaxMutations) {
        return false;
    }
    if (replacesRetainedScene(transaction)) {
        nodes.clear();
        rootId = 0;
    } else if (nodes.empty() || rootId == 0) {
        return false;
    }

    for (const auto& mutation : mutations) {
        if (mutation.reserved != 0 || mutation.node.id == 0) return false;
        const uint64_t id = mutation.node.id;
        switch (mutation.type) {
            case lcl::raster_protocol::NodeMutationType::CreateNode: {
                if (!validNodeState(mutation.node) || nodes.contains(id) ||
                    nodes.size() >= kMaxRetainedNodes) {
                    return false;
                }
                if (mutation.node.parentId == 0) {
                    if (rootId != 0) return false;
                    rootId = id;
                } else if (!nodes.contains(mutation.node.parentId)) {
                    return false;
                }
                nodes.emplace(id, mutation.node);
                break;
            }
            case lcl::raster_protocol::NodeMutationType::UpdateContent: {
                auto existing = nodes.find(id);
                if (existing == nodes.end() ||
                    mutation.node.contentRevision == 0) {
                    return false;
                }
                existing->second.contentRevision =
                    mutation.node.contentRevision;
                break;
            }
            case lcl::raster_protocol::NodeMutationType::SetProperties: {
                auto existing = nodes.find(id);
                if (existing == nodes.end() ||
                    !validNodeState(mutation.node)) {
                    return false;
                }
                const uint64_t contentRevision =
                    existing->second.contentRevision;
                existing->second = mutation.node;
                existing->second.contentRevision = contentRevision;
                break;
            }
            case lcl::raster_protocol::NodeMutationType::RemoveNode: {
                const auto existing = nodes.find(id);
                if (existing == nodes.end()) return false;
                if (std::any_of(
                        nodes.begin(), nodes.end(),
                        [id](const auto& entry) {
                            return entry.second.parentId == id;
                        })) {
                    return false;
                }
                if (rootId == id) rootId = 0;
                nodes.erase(existing);
                break;
            }
            default:
                return false;
        }
    }

    if (nodes.empty() || rootId == 0 || !nodes.contains(rootId)) return false;
    size_t rootCount = 0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> childOrders;
    for (const auto& [id, node] : nodes) {
        if (node.parentId == 0) {
            ++rootCount;
            if (id != rootId || node.siblingIndex != 0 ||
                (node.boundaryReasons & 1u) == 0) {
                return false;
            }
        } else if (!nodes.contains(node.parentId) ||
                   (node.boundaryReasons & 1u) != 0) {
            return false;
        } else {
            childOrders[node.parentId].push_back(node.siblingIndex);
        }

        uint64_t ancestor = id;
        size_t depth = 0;
        while (ancestor != rootId) {
            const auto current = nodes.find(ancestor);
            if (current == nodes.end() || current->second.parentId == 0 ||
                ++depth > nodes.size()) {
                return false;
            }
            ancestor = current->second.parentId;
        }
    }
    for (auto& [_, orders] : childOrders) {
        std::sort(orders.begin(), orders.end());
        for (size_t index = 0; index < orders.size(); ++index) {
            if (orders[index] != static_cast<uint32_t>(index)) return false;
        }
    }
    return rootCount == 1;
}

const RetainedNodeState* findRetainedNode(
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes,
        uint64_t id) noexcept {
    const auto found = nodes.find(id);
    return found == nodes.end() ? nullptr : &found->second;
}

bool isScrollTranslationProperties(
        const NodeMutation& mutation,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) noexcept {
    constexpr uint32_t kScrollContentReason = 1u << 5u;
    constexpr uint32_t kTransformReason = 1u << 2u;
    constexpr float kEpsilon = 0.0001f;
    const auto same = [=](float lhs, float rhs) {
        return std::fabs(lhs - rhs) <= kEpsilon;
    };
    if (mutation.type !=
            lcl::raster_protocol::NodeMutationType::SetProperties) {
        return false;
    }
    const auto& node = mutation.node;
    const auto* old = findRetainedNode(nodes, node.id);
    if (!old) return false;
    const float translationDeltaX = node.translationX - old->translationX;
    const float translationDeltaY = node.translationY - old->translationY;
    return validNodeState(node) &&
        (node.boundaryReasons & kScrollContentReason) != 0 &&
        (old->boundaryReasons & kScrollContentReason) != 0 &&
        ((node.boundaryReasons ^ old->boundaryReasons) &
        ~kTransformReason) == 0 &&
        node.parentId == old->parentId &&
        node.siblingIndex == old->siblingIndex &&
        node.flags == old->flags &&
        node.externalBufferId == old->externalBufferId &&
        node.externalBufferRevision == old->externalBufferRevision &&
        same(node.layoutX, old->layoutX) &&
        same(node.layoutY, old->layoutY) &&
        same(node.layoutWidth, old->layoutWidth) &&
        same(node.layoutHeight, old->layoutHeight) &&
        same(node.presentationX - old->presentationX, translationDeltaX) &&
        same(node.presentationY - old->presentationY, translationDeltaY) &&
        same(node.presentationWidth, old->presentationWidth) &&
        same(node.presentationHeight, old->presentationHeight) &&
        same(node.clipX, old->clipX) &&
        same(node.clipY, old->clipY) &&
        same(node.clipWidth, old->clipWidth) &&
        same(node.clipHeight, old->clipHeight) &&
        same(node.opacity, 1.0f) && same(old->opacity, 1.0f) &&
        same(node.scaleX, 1.0f) && same(node.scaleY, 1.0f) &&
        same(old->scaleX, 1.0f) && same(old->scaleY, 1.0f) &&
        same(node.rotationRadians, 0.0f) &&
        same(old->rotationRadians, 0.0f) &&
        same(node.originX, old->originX) &&
        same(node.originY, old->originY);
}

bool isScrollTransformOnly(
        const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) noexcept {
    if (replacesRetainedScene(transaction) || mutations.empty()) return false;
    return std::all_of(
        mutations.begin(), mutations.end(), [&](const NodeMutation& mutation) {
            return isScrollTranslationProperties(mutation, nodes);
        });
}

bool isRetainedPresentationCandidate(
        const RetainedNodeState& node) noexcept {
    constexpr uint32_t kRootReason = 1u << 0u;
    constexpr uint32_t kTransformReason = 1u << 2u;
    constexpr uint32_t kOpacityReason = 1u << 3u;
    constexpr uint32_t kScrollViewportReason = 1u << 4u;
    constexpr uint32_t kScrollContentReason = 1u << 5u;
    constexpr uint32_t kExternalBufferReason = 1u << 7u;
    const bool presentation =
        (node.boundaryReasons &
         (kTransformReason | kOpacityReason)) != 0;
    const float area = node.layoutWidth * node.layoutHeight;
    return presentation &&
        (node.boundaryReasons & kRootReason) == 0 &&
        (node.boundaryReasons &
         (kScrollViewportReason | kScrollContentReason)) == 0 &&
        (node.boundaryReasons & kExternalBufferReason) == 0 &&
        node.layoutWidth > 0.0f && node.layoutHeight > 0.0f &&
        node.layoutWidth <= 2048.0f && node.layoutHeight <= 2048.0f &&
        area <= 4194304.0f;
}

bool isScrollViewport(const RetainedNodeState& node) noexcept {
    return (node.boundaryReasons & (1u << 4u)) != 0;
}

bool isScrollContent(const RetainedNodeState& node) noexcept {
    return (node.boundaryReasons & (1u << 5u)) != 0;
}

bool isExternalBuffer(const RetainedNodeState& node) noexcept {
    return (node.boundaryReasons & (1u << 7u)) != 0;
}

bool isWithinScrollContent(
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes,
        uint64_t id) noexcept {
    const auto* node = findRetainedNode(nodes, id);
    size_t depth = 0;
    while (node && depth++ <= nodes.size()) {
        if (isScrollContent(*node)) return true;
        if (node->parentId == 0) return false;
        node = findRetainedNode(nodes, node->parentId);
    }
    return false;
}

bool descendsFrom(
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes,
        uint64_t id, uint64_t ancestorId) noexcept {
    const auto* node = findRetainedNode(nodes, id);
    size_t depth = 0;
    while (node && depth++ <= nodes.size()) {
        if (node->parentId == ancestorId) return true;
        if (node->parentId == 0) return false;
        node = findRetainedNode(nodes, node->parentId);
    }
    return false;
}

std::unordered_set<uint64_t> retainedPresentationNodeIds(
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) {
    std::unordered_set<uint64_t> result;
    for (const auto& [id, node] : nodes) {
        if (!isRetainedPresentationCandidate(node) ||
            isWithinScrollContent(nodes, id)) {
            continue;
        }
        bool hasCandidateAncestor = false;
        for (const auto& [ancestorId, ancestor] : nodes) {
            if (ancestorId != id &&
                isRetainedPresentationCandidate(ancestor) &&
                descendsFrom(nodes, id, ancestorId)) {
                hasCandidateAncestor = true;
                break;
            }
        }
        if (hasCandidateAncestor) continue;
        const bool containsScroll = std::any_of(
            nodes.begin(), nodes.end(), [&](const auto& descendant) {
                return (isScrollViewport(descendant.second) ||
                        isScrollContent(descendant.second)) &&
                    descendsFrom(nodes, descendant.first, id);
            });
        const bool containsExternal = std::any_of(
            nodes.begin(), nodes.end(), [&](const auto& descendant) {
                return isExternalBuffer(descendant.second) &&
                    (descendant.first == id ||
                     descendsFrom(nodes, descendant.first, id));
            });
        if (!containsScroll && !containsExternal) result.insert(id);
    }
    return result;
}

bool isRetainedPresentationProperties(
        const NodeMutation& mutation,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes,
        const std::unordered_set<uint64_t>& candidates) noexcept {
    constexpr uint32_t kTransformReason = 1u << 2u;
    constexpr uint32_t kOpacityReason = 1u << 3u;
    constexpr float kEpsilon = 0.0001f;
    const auto same = [=](float lhs, float rhs) {
        return std::fabs(lhs - rhs) <= kEpsilon;
    };
    if (mutation.type !=
            lcl::raster_protocol::NodeMutationType::SetProperties ||
        !candidates.contains(mutation.node.id)) {
        return false;
    }
    const auto& node = mutation.node;
    const auto* old = findRetainedNode(nodes, node.id);
    if (!old || !validNodeState(node)) return false;
    const uint32_t allowedReasons = kTransformReason | kOpacityReason;
    return isRetainedPresentationCandidate(node) &&
        ((node.boundaryReasons ^ old->boundaryReasons) &
         ~allowedReasons) == 0 &&
        node.parentId == old->parentId &&
        node.siblingIndex == old->siblingIndex &&
        node.flags == old->flags &&
        node.externalBufferId == old->externalBufferId &&
        node.externalBufferRevision == old->externalBufferRevision &&
        node.contentRevision == old->contentRevision &&
        same(node.layoutX, old->layoutX) &&
        same(node.layoutY, old->layoutY) &&
        same(node.layoutWidth, old->layoutWidth) &&
        same(node.layoutHeight, old->layoutHeight) &&
        same(node.clipX, old->clipX) &&
        same(node.clipY, old->clipY) &&
        same(node.clipWidth, old->clipWidth) &&
        same(node.clipHeight, old->clipHeight);
}

bool isRetainedPresentationOnly(
        const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) {
    if (replacesRetainedScene(transaction) || mutations.empty()) return false;
    const auto candidates = retainedPresentationNodeIds(nodes);
    return !candidates.empty() && std::all_of(
        mutations.begin(), mutations.end(), [&](const NodeMutation& mutation) {
            return isRetainedPresentationProperties(
                mutation, nodes, candidates);
        });
}

bool isExternalBufferProperties(
        const NodeMutation& mutation,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) noexcept {
    constexpr float kEpsilon = 0.0001f;
    const auto same = [=](float lhs, float rhs) {
        return std::fabs(lhs - rhs) <= kEpsilon;
    };
    if (mutation.type !=
            lcl::raster_protocol::NodeMutationType::SetProperties) {
        return false;
    }
    const auto& node = mutation.node;
    const auto* old = findRetainedNode(nodes, node.id);
    if (!old || !validNodeState(node) || !isExternalBuffer(node) ||
        !isExternalBuffer(*old) || isWithinScrollContent(nodes, node.id)) {
        return false;
    }
    return node.parentId == old->parentId &&
        node.siblingIndex == old->siblingIndex &&
        node.boundaryReasons == old->boundaryReasons &&
        ((node.flags ^ old->flags) &
         ~lcl::raster_protocol::kNodeHasExternalBuffer) == 0 &&
        node.contentRevision == old->contentRevision &&
        same(node.layoutX, old->layoutX) &&
        same(node.layoutY, old->layoutY) &&
        same(node.layoutWidth, old->layoutWidth) &&
        same(node.layoutHeight, old->layoutHeight) &&
        same(node.presentationX, old->presentationX) &&
        same(node.presentationY, old->presentationY) &&
        same(node.presentationWidth, old->presentationWidth) &&
        same(node.presentationHeight, old->presentationHeight) &&
        same(node.clipX, old->clipX) && same(node.clipY, old->clipY) &&
        same(node.clipWidth, old->clipWidth) &&
        same(node.clipHeight, old->clipHeight) &&
        same(node.opacity, old->opacity) &&
        same(node.translationX, old->translationX) &&
        same(node.translationY, old->translationY) &&
        same(node.scaleX, old->scaleX) && same(node.scaleY, old->scaleY) &&
        same(node.rotationRadians, old->rotationRadians) &&
        same(node.originX, old->originX) && same(node.originY, old->originY) &&
        (node.externalBufferId != old->externalBufferId ||
         node.externalBufferRevision != old->externalBufferRevision);
}

bool isExternalBufferOnly(
        const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) noexcept {
    return !replacesRetainedScene(transaction) && !mutations.empty() &&
        std::all_of(
            mutations.begin(), mutations.end(), [&](const auto& mutation) {
                return isExternalBufferProperties(mutation, nodes);
            });
}

bool isRetainedPropertyOnly(
        const CommitTransaction& transaction,
        std::span<const NodeMutation> mutations,
        const std::unordered_map<uint64_t, RetainedNodeState>& nodes) {
    if (replacesRetainedScene(transaction) || mutations.empty()) return false;
    const auto candidates = retainedPresentationNodeIds(nodes);
    return std::all_of(
        mutations.begin(), mutations.end(), [&](const NodeMutation& mutation) {
            return isScrollTranslationProperties(mutation, nodes) ||
                isExternalBufferProperties(mutation, nodes) ||
                isRetainedPresentationProperties(
                    mutation, nodes, candidates);
        });
}

bool invalidatesRetainedCompositionTemplate(
        std::span<const NodeMutation> mutations,
        const std::unordered_map<uint64_t, RetainedNodeState>& currentNodes,
        const std::unordered_map<uint64_t, RetainedNodeState>& nextNodes)
        noexcept {
    const auto currentPresentation =
        retainedPresentationNodeIds(currentNodes);
    const auto nextPresentation = retainedPresentationNodeIds(nextNodes);
    const auto isWithinPresentation = [](
            const auto& nodes, const auto& candidates, uint64_t id) {
        return std::any_of(
            candidates.begin(), candidates.end(), [&](uint64_t candidate) {
                return candidate == id ||
                    descendsFrom(nodes, id, candidate);
            });
    };
    for (const auto& mutation : mutations) {
        if (mutation.type ==
                lcl::raster_protocol::NodeMutationType::RemoveNode) {
            const auto* existing = findRetainedNode(
                currentNodes, mutation.node.id);
            if (existing &&
                isWithinPresentation(
                    currentNodes, currentPresentation,
                    mutation.node.id)) {
                if (currentPresentation.contains(mutation.node.id)) {
                    return true;
                }
                continue;
            }
            if (!existing || isScrollViewport(*existing) ||
                isScrollContent(*existing) ||
                !isWithinScrollContent(currentNodes, mutation.node.id)) {
                return true;
            }
            continue;
        }
        const auto* next = findRetainedNode(nextNodes, mutation.node.id);
        if (!next || isScrollViewport(*next)) return true;
        if (isWithinPresentation(
                nextNodes, nextPresentation, mutation.node.id)) {
            if (nextPresentation.contains(mutation.node.id)) {
                if (mutation.type ==
                        lcl::raster_protocol::NodeMutationType::CreateNode) {
                    return true;
                }
                if (mutation.type ==
                        lcl::raster_protocol::NodeMutationType::SetProperties &&
                    !isRetainedPresentationProperties(
                        mutation, currentNodes, currentPresentation)) {
                    return true;
                }
            }
            continue;
        }
        if (isScrollContent(*next)) {
            if (mutation.type ==
                    lcl::raster_protocol::NodeMutationType::CreateNode ||
                (mutation.type ==
                     lcl::raster_protocol::NodeMutationType::SetProperties &&
                 !isScrollTranslationProperties(
                     mutation, currentNodes))) {
                return true;
            }
            continue;
        }
        if (isExternalBufferProperties(mutation, currentNodes)) {
            continue;
        }
        if (!isWithinScrollContent(nextNodes, mutation.node.id)) return true;
    }
    return false;
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int createListener(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }
#if !defined(__ANDROID__)
    std::string ownershipError;
    if (!lcl::security::assignSessionUserOwnership(path, 0600, ownershipError)) {
        std::cerr << "[LCL Rasterd ERROR] " << ownershipError << "\n";
        close(fd);
        unlink(path.c_str());
        return -1;
    }
#else
    if (chmod(path.c_str(), 0600) != 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }
#endif
    if (listen(fd, 32) != 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    return fd;
}

class RasterService {
public:
    RasterService(int compositorFd, int nativeBufferFd, std::string socketPath)
        : m_compositorFd(compositorFd),
          m_nativeBufferFd(nativeBufferFd),
          m_socketPath(std::move(socketPath)) {}

    ~RasterService() {
        for (auto& frame : m_systemFrames) if (frame.displayListFd >= 0) close(frame.displayListFd);
        for (auto& frame : m_appFrames) if (frame.displayListFd >= 0) close(frame.displayListFd);
        for (const auto& client : m_clients) if (client.fd >= 0) close(client.fd);
        if (m_listenerFd >= 0) close(m_listenerFd);
        if (m_compositorFd >= 0) close(m_compositorFd);
        if (m_nativeBufferFd >= 0) close(m_nativeBufferFd);
        unlink(m_socketPath.c_str());
    }

    bool initialize() {
        if (m_compositorFd < 0 || !setNonBlocking(m_compositorFd)) return false;
#if defined(__ANDROID__)
        // LayerReady is published only after the AHB handle is queued, so the
        // compositor receives from this trusted sideband only when data is
        // guaranteed. Keep the NDK handle-transfer socket blocking so one
        // opaque native-handle transaction completes before publication.
        if (m_nativeBufferFd < 0) return false;
#endif
        m_listenerFd = createListener(m_socketPath);
        if (m_listenerFd < 0) return false;
        return lcl::raster_protocol::sendPacket(
            m_compositorFd, Opcode::Ready,
            static_cast<const void*>(nullptr), 0u, -1);
    }

    int run() {
        while (m_running) {
            acceptClients();
            pollCompositor();
            pollClients();
            renderOne();

            std::vector<pollfd> descriptors;
            descriptors.push_back({m_compositorFd, POLLIN, 0});
            descriptors.push_back({m_listenerFd, POLLIN, 0});
            for (const auto& client : m_clients) {
                descriptors.push_back({client.fd, POLLIN | POLLHUP, 0});
            }
            (void)poll(descriptors.data(), descriptors.size(),
                       hasPendingFrames() ? 0 : 4);
        }
        return 0;
    }

private:
    void acceptClients() {
        while (true) {
            const int fd = accept4(m_listenerFd, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
                close(fd);
                continue;
            }
            m_clients.push_back({fd, credentials.pid});
        }
    }

    void pollCompositor() {
        while (true) {
            lcl::raster_protocol::Header header{};
            std::vector<uint8_t> payload;
            int receivedFd = -1;
            const auto status = lcl::raster_protocol::receivePacket(
                m_compositorFd, header, payload, receivedFd);
            if (status == lcl::raster_protocol::ReceiveStatus::WouldBlock) break;
            if (status == lcl::raster_protocol::ReceiveStatus::Closed ||
                status == lcl::raster_protocol::ReceiveStatus::Error) {
                if (receivedFd >= 0) close(receivedFd);
                m_running = false;
                break;
            }
            if (status != lcl::raster_protocol::ReceiveStatus::Received) {
                if (receivedFd >= 0) close(receivedFd);
                continue;
            }
            if (const auto* grant = lcl::raster_protocol::payloadAs<SurfaceGrant>(
                    header, payload, Opcode::RegisterSurface)) {
                auto& surface = m_surfaces[tokenOf(*grant)];
                surface.grant = *grant;
            } else if (const auto* grant = lcl::raster_protocol::payloadAs<SurfaceGrant>(
                           header, payload, Opcode::RevokeSurface)) {
                revokeSurface(tokenOf(*grant));
            } else if (const auto* release = lcl::raster_protocol::payloadAs<
                           lcl::raster_protocol::ReleaseLayer>(
                           header, payload, Opcode::ReleaseLayer)) {
                releaseLayer(
                    release->layerId, release->reason,
                    std::exchange(receivedFd, -1));
            }
            if (receivedFd >= 0) close(receivedFd);
        }
    }

    void pollClients() {
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            bool alive = true;
            while (alive) {
                lcl::raster_protocol::Header header{};
                std::vector<uint8_t> payload;
                int receivedFd = -1;
                const auto status = lcl::raster_protocol::receivePacket(
                    it->fd, header, payload, receivedFd);
                if (status == lcl::raster_protocol::ReceiveStatus::WouldBlock) break;
                if (status == lcl::raster_protocol::ReceiveStatus::Closed ||
                    status == lcl::raster_protocol::ReceiveStatus::Error) {
                    if (receivedFd >= 0) close(receivedFd);
                    alive = false;
                    break;
                }
                if (status != lcl::raster_protocol::ReceiveStatus::Received) {
                    if (receivedFd >= 0) close(receivedFd);
                    continue;
                }
                handleClientPacket(*it, header, payload, receivedFd);
            }
            if (!alive) {
                for (auto& [_, surface] : m_surfaces) {
                    for (auto& [key, resource] : surface.externalBuffers) {
                        (void)key;
                        if (resource.clientFd == it->fd) {
                            resource.clientFd = -1;
                        }
                    }
                }
                close(it->fd);
                it = m_clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    SurfaceState* authorizedSurface(const Client& client,
                                    const SurfaceGrant& grant) {
        const auto found = m_surfaces.find(tokenOf(grant));
        if (found == m_surfaces.end() || found->second.grant.surfaceId != grant.surfaceId ||
            found->second.grant.ownerPid != client.pid ||
            grant.ownerPid != client.pid ||
            found->second.grant.flags != grant.flags) {
            return nullptr;
        }
        return &found->second;
    }

    void handleClientPacket(const Client& client,
                            const lcl::raster_protocol::Header& header,
                            std::span<const uint8_t> payload, int receivedFd) {
        if (const auto* upload = lcl::raster_protocol::payloadAs<
                lcl::raster_protocol::UploadImage>(
                header, payload, Opcode::UploadImage)) {
            auto* surface = authorizedSurface(client, upload->grant);
            if (!surface || !isImmutableMemfd(receivedFd, upload->byteSize) ||
                upload->width == 0 || upload->height == 0 ||
                upload->stridePixels < upload->width ||
                upload->byteSize != static_cast<uint64_t>(upload->stridePixels) *
                    upload->height * sizeof(uint32_t)) {
                if (receivedFd >= 0) close(receivedFd);
                return;
            }
            ImageResource resource{};
            resource.rendererId = m_nextImageId++;
            resource.width = upload->width;
            resource.height = upload->height;
            resource.stridePixels = upload->stridePixels;
            resource.opaque = upload->opaque != 0;
            resource.pixels.resize(
                static_cast<size_t>(upload->stridePixels) * upload->height);
            // Android may reject a cross-domain MAP_SHARED mapping of a
            // sealed memfd even though SCM_RIGHTS and pread are permitted.
            // Reading the immutable payload also avoids retaining a mapping
            // to producer-owned input beyond this packet.
            if (readExact(receivedFd, resource.pixels.data(), upload->byteSize)) {
                surface->images[{upload->resourceId, upload->contentRevision}] =
                    std::move(resource);
            }
            close(receivedFd);
            return;
        }

        if (const auto* upload = lcl::raster_protocol::payloadAs<
                lcl::raster_protocol::UploadExternalBuffer>(
                header, payload, Opcode::UploadExternalBuffer)) {
            auto* surface = authorizedSurface(client, upload->grant);
            constexpr uint32_t kMaxDimension = 16384;
            constexpr uint64_t kMaxByteSize =
                512ull * 1024ull * 1024ull;
            const ExternalKey key{upload->bufferId, upload->contentRevision};
            if (surface) {
                const auto existing = surface->externalBuffers.find(key);
                if (existing != surface->externalBuffers.end() &&
                    existing->second.transport == upload->transport &&
                    existing->second.width == upload->width &&
                    existing->second.height == upload->height &&
                    existing->second.stride == upload->stride &&
                    existing->second.format == upload->format &&
                    existing->second.modifier == upload->modifier) {
                    existing->second.clientFd = client.fd;
                    if (receivedFd >= 0) close(receivedFd);
                    return;
                }
            }
            const bool commonValid = surface && receivedFd >= 0 &&
                upload->bufferId != 0 && upload->contentRevision != 0 &&
                upload->width > 0 && upload->height > 0 &&
                upload->width <= kMaxDimension &&
                upload->height <= kMaxDimension &&
                upload->stride >= upload->width * sizeof(uint32_t) &&
                upload->stride % sizeof(uint32_t) == 0 &&
                upload->stride <= kMaxDimension * sizeof(uint32_t) &&
                upload->format == lcl::platform::kDmaBufFormatArgb8888 &&
                upload->byteSize <= kMaxByteSize &&
                upload->flags == 0 && upload->reserved == 0 &&
                !surface->externalBuffers.contains(key);
            const bool shm = upload->transport ==
                lcl::raster_protocol::ExternalBufferTransport::
                    ImmutableShmArgb8888;
            const bool dmaBuf = upload->transport ==
                lcl::raster_protocol::ExternalBufferTransport::DmaBufArgb8888;
            const bool transportValid = shm
                ? upload->byteSize ==
                      static_cast<uint64_t>(upload->stride) * upload->height &&
                      isImmutableMemfd(receivedFd, upload->byteSize)
                : dmaBuf && upload->byteSize == 0;
            if (!commonValid || !transportValid) {
                if (receivedFd >= 0) close(receivedFd);
                lcl::raster_protocol::ExternalBufferReleased released{};
                released.bufferId = upload->bufferId;
                released.contentRevision = upload->contentRevision;
                released.reason =
                    lcl::raster_protocol::ExternalBufferReleaseReason::Rejected;
                (void)lcl::raster_protocol::sendPacket(
                    client.fd, Opcode::ExternalBufferReleased, released);
                return;
            }

            ExternalBufferResource resource{};
            resource.transport = upload->transport;
            resource.width = upload->width;
            resource.height = upload->height;
            resource.stride = upload->stride;
            resource.format = upload->format;
            resource.modifier = upload->modifier;
            resource.clientFd = client.fd;
            if (shm) {
                resource.pixels.resize(
                    static_cast<size_t>(upload->stride / sizeof(uint32_t)) *
                    upload->height);
                if (!readExact(
                        receivedFd, resource.pixels.data(), upload->byteSize)) {
                    close(receivedFd);
                    lcl::raster_protocol::ExternalBufferReleased released{};
                    released.bufferId = upload->bufferId;
                    released.contentRevision = upload->contentRevision;
                    released.reason =
                        lcl::raster_protocol::ExternalBufferReleaseReason::Rejected;
                    (void)lcl::raster_protocol::sendPacket(
                        client.fd, Opcode::ExternalBufferReleased, released);
                    return;
                }
                close(receivedFd);
            } else {
                resource.bufferFd = receivedFd;
            }
            surface->externalBuffers.emplace(key, std::move(resource));
            return;
        }

        if (const auto* fence = lcl::raster_protocol::payloadAs<
                lcl::raster_protocol::SetExternalBufferFence>(
                header, payload, Opcode::SetExternalBufferFence)) {
            auto* surface = authorizedSurface(client, fence->grant);
            const ExternalKey key{fence->bufferId, fence->contentRevision};
            if (!surface || receivedFd < 0) {
                if (receivedFd >= 0) close(receivedFd);
                return;
            }
            const auto found = surface->externalBuffers.find(key);
            if (found == surface->externalBuffers.end() ||
                found->second.transport !=
                    lcl::raster_protocol::ExternalBufferTransport::
                        DmaBufArgb8888) {
                close(receivedFd);
                return;
            }
            if (found->second.acquireFenceFd >= 0) {
                close(found->second.acquireFenceFd);
            }
            found->second.acquireFenceFd = receivedFd;
            return;
        }

        CommitTransaction transaction{};
        std::vector<NodeMutation> mutations;
        if (lcl::raster_protocol::decodeCommitTransaction(
                header, payload, transaction, mutations)) {
            auto* surface = authorizedSurface(client, transaction.grant);
            const bool hasDisplayList = transaction.displayListSize != 0;
            const bool validDisplayListDescriptor = hasDisplayList
                ? isImmutableMemfd(receivedFd, transaction.displayListSize)
                : receivedFd < 0;
            const bool validPropertyOnlyCommit = hasDisplayList ||
                (surface && isRetainedPropertyOnly(
                    transaction, mutations, surface->retainedNodes));
            if (!surface ||
                !hasValidRetainedFrameMetadata(transaction) ||
                !validDisplayListDescriptor || !validPropertyOnlyCommit ||
                (replacesRetainedScene(transaction) && !hasDisplayList)) {
                if (receivedFd >= 0) close(receivedFd);
                discard(client.fd, transaction,
                        surface ? lcl::raster_protocol::DiscardReason::InvalidFrame
                                : lcl::raster_protocol::DiscardReason::InvalidGrant);
                return;
            }
            auto& queue = (transaction.grant.flags &
                           lcl::raster_protocol::kGrantInteractiveSystem) != 0
                ? m_systemFrames : m_appFrames;
            auto existing = std::find_if(queue.begin(), queue.end(), [&transaction](const auto& frame) {
                return tokenOf(frame.transaction.grant) ==
                    tokenOf(transaction.grant);
            });
            if (existing != queue.end()) {
                if (replacesRetainedScene(transaction)) {
                    // A complete replacement has no dependency on the queued
                    // base and may safely collapse obsolete work.
                    discard(existing->clientFd, existing->transaction,
                            lcl::raster_protocol::DiscardReason::Superseded);
                    if (existing->displayListFd >= 0) {
                        close(existing->displayListFd);
                    }
                    *existing = {
                        client.fd, transaction, std::move(mutations),
                        receivedFd};
                } else {
                    // Retained patches are ordered against an exact base.
                    // Never drop the queued predecessor and apply this patch
                    // to a different scene.
                    discard(client.fd, transaction,
                            lcl::raster_protocol::DiscardReason::InvalidFrame);
                    if (receivedFd >= 0) close(receivedFd);
                }
            } else {
                queue.push_back({
                    client.fd, transaction, std::move(mutations), receivedFd});
            }
            return;
        }

        if (receivedFd >= 0) close(receivedFd);
    }

    bool hasPendingFrames() const {
        return !m_systemFrames.empty() || !m_appFrames.empty();
    }

    void renderOne() {
        std::deque<PendingFrame>* queue = nullptr;
        if (!m_systemFrames.empty() && (m_systemBurst < 3 || m_appFrames.empty())) {
            queue = &m_systemFrames;
            ++m_systemBurst;
        } else if (!m_appFrames.empty()) {
            queue = &m_appFrames;
            m_systemBurst = 0;
        }
        if (!queue) return;

        auto frame = std::move(queue->front());
        queue->pop_front();
        auto surfaceIt = m_surfaces.find(tokenOf(frame.transaction.grant));
        if (surfaceIt == m_surfaces.end()) {
            discard(frame.clientFd, frame.transaction,
                    lcl::raster_protocol::DiscardReason::SurfaceRevoked);
            if (frame.displayListFd >= 0) close(frame.displayListFd);
            return;
        }
        auto nextNodes = surfaceIt->second.retainedNodes;
        uint64_t nextRootId = surfaceIt->second.retainedRootId;
        if (!applyNodeMutations(
                frame.transaction, frame.mutations,
                nextNodes, nextRootId)) {
            discard(frame.clientFd, frame.transaction,
                    lcl::raster_protocol::DiscardReason::InvalidFrame);
            if (frame.displayListFd >= 0) close(frame.displayListFd);
            return;
        }
        if (!rasterFrame(
                surfaceIt->second, frame.transaction,
                frame.mutations, nextNodes, frame.displayListFd)) {
            discard(frame.clientFd, frame.transaction,
                    lcl::raster_protocol::DiscardReason::RasterFailure);
        } else {
            // Retained-node state and the immutable raster output become
            // visible together; a failed replay never advances either base.
            surfaceIt->second.retainedNodes = std::move(nextNodes);
            surfaceIt->second.retainedRootId = nextRootId;
            pruneExternalBuffers(surfaceIt->second);
        }
        if (frame.displayListFd >= 0) close(frame.displayListFd);
    }

    LayerSlot* acquireSlot(
            SurfaceState& surface, const CommitTransaction& submit) {
        const uint32_t width = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalWidth * submit.bufferScale)));
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalHeight * submit.bufferScale)));
        const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
        for (auto& slot : surface.slots) {
            if (slot.busy) continue;
            if (slot.bytes != bytes) {
                slot.release();
                slot.fd = createMemfd("lcl-raster-layer", bytes);
                if (slot.fd < 0) return nullptr;
                void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, slot.fd, 0);
                if (mapping == MAP_FAILED) {
                    slot.release();
                    return nullptr;
                }
                slot.pixels = static_cast<uint32_t*>(mapping);
                slot.bytes = bytes;
                slot.width = width;
                slot.height = height;
                // A daemon restart must never recycle an identifier still
                // retained by the compositor from the previous process.
                slot.layerId =
                    (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
                    m_nextLayerId++;
            }
            slot.busy = true;
            return &slot;
        }
        return nullptr;
    }

    bool rasterFrame(SurfaceState& surface,
                     const CommitTransaction& submit,
                     std::span<const NodeMutation> mutations,
                     const std::unordered_map<
                         uint64_t, RetainedNodeState>& nextNodes,
                     int displayListFd) {
        const uint64_t rasterStartNs = submit.clientFrameStartNs != 0
            ? monotonicNowNs() : 0;
        if (!replacesRetainedScene(submit) &&
            !retainedBaseMatches(
                submit, surface.gpuFrameSerial,
                surface.gpuGeometryGeneration, surface.gpuWidth,
                surface.gpuHeight, surface.gpuScale) &&
            !retainedBaseMatches(
                submit, surface.softwareFrameSerial,
                surface.softwareGeometryGeneration, surface.softwareWidth,
                surface.softwareHeight, surface.softwareScale)) {
            return false;
        }
        auto nextLayerNamespaces = surface.cachedLayerNamespaces;
        std::vector<uint64_t> genericLayerEvictions;
        if (replacesRetainedScene(submit)) {
            genericLayerEvictions.reserve(nextLayerNamespaces.size());
            for (const auto& [_, layerId] : nextLayerNamespaces) {
                if (layerId != 0) genericLayerEvictions.push_back(layerId);
            }
            nextLayerNamespaces.clear();
        } else {
            for (const NodeMutation& mutation : mutations) {
                if (mutation.type !=
                    lcl::raster_protocol::NodeMutationType::RemoveNode) {
                    continue;
                }
                const auto layer = nextLayerNamespaces.find(mutation.node.id);
                if (layer == nextLayerNamespaces.end()) continue;
                if (layer->second != 0) {
                    genericLayerEvictions.push_back(layer->second);
                }
                nextLayerNamespaces.erase(layer);
            }
        }
        const auto retainedPresentationIds =
            retainedPresentationNodeIds(nextNodes);
        for (auto layer = nextLayerNamespaces.begin();
             layer != nextLayerNamespaces.end();) {
            const auto node = nextNodes.find(layer->first);
            const bool activeScrollOwner = node != nextNodes.end() &&
                isScrollViewport(node->second);
            const bool activePresentationOwner =
                retainedPresentationIds.contains(layer->first);
            if (node != nextNodes.end() &&
                (activeScrollOwner || activePresentationOwner)) {
                ++layer;
                continue;
            }
            if (layer->second != 0) {
                genericLayerEvictions.push_back(layer->second);
            }
            layer = nextLayerNamespaces.erase(layer);
        }
        std::vector<uint64_t> createdNamespaceLayers;
        std::shared_ptr<std::vector<lcl::graphics::DisplayCommand>> commands;
        if (submit.displayListSize != 0) {
            std::vector<uint8_t> wireBytes(submit.displayListSize);
            if (!readExact(
                    displayListFd, wireBytes.data(), wireBytes.size())) {
                return false;
            }
            const auto wire = std::span<const uint8_t>(wireBytes);
            auto decoded = lcl::graphics::decodeDisplayList(
                wire, submit.displayListSize);
            if (!decoded) return false;

            commands = std::make_shared<std::vector<
                lcl::graphics::DisplayCommand>>(
                    decoded.displayList.commands());
            for (auto& command : *commands) {
                if (auto* image = std::get_if<
                        lcl::graphics::DrawImageCommand>(&command)) {
                    const auto found = surface.images.find(
                        {image->resourceId, image->contentRevision});
                    if (found == surface.images.end()) return false;
                    auto& resource = found->second;
                    image->resourceKey = reinterpret_cast<uintptr_t>(
                        resource.pixels.data());
                    image->resourceId = resource.rendererId;
                    image->sourceWidth = static_cast<int>(resource.width);
                    image->sourceHeight = static_cast<int>(resource.height);
                    image->stridePixels =
                        static_cast<int>(resource.stridePixels);
                    image->opaque = resource.opaque;
                } else if (auto* begin = std::get_if<
                               lcl::graphics::BeginCachedLayerCommand>(
                                   &command)) {
                    auto [found, inserted] =
                        nextLayerNamespaces.try_emplace(
                            begin->id, 0);
                    if (inserted) {
                        found->second = m_nextCachedLayerId++;
                        createdNamespaceLayers.push_back(found->second);
                    }
                    begin->id = found->second;
                } else if (auto* draw = std::get_if<
                               lcl::graphics::DrawCachedLayerCommand>(
                                   &command)) {
                    auto [found, inserted] =
                        nextLayerNamespaces.try_emplace(
                            draw->id, 0);
                    if (inserted) {
                        found->second = m_nextCachedLayerId++;
                        createdNamespaceLayers.push_back(found->second);
                    }
                    draw->id = found->second;
                }
            }
        }

        const bool scrollTransformTransaction = isScrollTransformOnly(
            submit, mutations, surface.retainedNodes);
        const bool retainedPresentationTransaction =
            isRetainedPresentationOnly(
                submit, mutations, surface.retainedNodes);
        const bool externalBufferTransaction = isExternalBufferOnly(
            submit, mutations, surface.retainedNodes);
        const bool invalidatesRetainedTemplate =
            invalidatesRetainedCompositionTemplate(
                mutations, surface.retainedNodes, nextNodes);
        auto tiled = lcl::render::RetainedScrollTileCache::prepare(
            surface.scrollTiles,
            commands ? commands.get() : nullptr,
            nextNodes, nextLayerNamespaces,
            [this] { return m_nextCachedLayerId++; },
            replacesRetainedScene(submit),
            scrollTransformTransaction || retainedPresentationTransaction ||
                externalBufferTransaction,
            invalidatesRetainedTemplate);
        if (!tiled) return false;
        auto resolvedCommands = std::make_shared<std::vector<
            lcl::graphics::DisplayCommand>>(tiled->displayList.commands());
        bool requiresGpuExternal = false;
        for (auto& command : *resolvedCommands) {
            auto* external = std::get_if<
                lcl::graphics::DrawExternalBufferCommand>(&command);
            if (!external) continue;
            const auto node = nextNodes.find(external->nodeId);
            if (node == nextNodes.end() || !isExternalBuffer(node->second)) {
                return false;
            }
            external->resourceKey = 0;
            external->bufferId = node->second.externalBufferId;
            external->contentRevision =
                node->second.externalBufferRevision;
            external->sourceWidth = 0;
            external->sourceHeight = 0;
            external->stridePixels = 0;
            external->sampleKind =
                lcl::graphics::ExternalBufferSampleKind::Unresolved;
            if (external->bufferId == 0 || external->contentRevision == 0) {
                continue;
            }
            const auto resource = surface.externalBuffers.find({
                external->bufferId, external->contentRevision});
            if (resource == surface.externalBuffers.end()) return false;
            external->sourceWidth = static_cast<int>(resource->second.width);
            external->sourceHeight = static_cast<int>(resource->second.height);
            if (resource->second.transport ==
                    lcl::raster_protocol::ExternalBufferTransport::
                        ImmutableShmArgb8888) {
                external->resourceKey = reinterpret_cast<uintptr_t>(
                    resource->second.pixels.data());
                external->stridePixels = static_cast<int>(
                    resource->second.stride / sizeof(uint32_t));
                external->sampleKind =
                    lcl::graphics::ExternalBufferSampleKind::ArgbPixels;
            } else {
                requiresGpuExternal = true;
            }
        }
        const auto immutableResolved = std::shared_ptr<const std::vector<
            lcl::graphics::DisplayCommand>>(resolvedCommands);
        const lcl::graphics::DisplayList displayList(immutableResolved);
        const auto releaseCachedLayers = [&](std::span<const uint64_t> ids) {
            for (const uint64_t id : ids) {
                if (surface.gpuRenderer) {
                    surface.gpuRenderer->releaseCachedDisplayLayer(id);
                }
                if (surface.softwareRenderer) {
                    surface.softwareRenderer->releaseCachedDisplayLayer(id);
                }
            }
        };
        const auto commitTiledState = [&] {
            releaseCachedLayers(tiled->evictedLayerIds);
            releaseCachedLayers(genericLayerEvictions);
            surface.scrollTiles = std::move(*tiled->next);
            surface.cachedLayerNamespaces = std::move(nextLayerNamespaces);
        };
        const auto discardPreparedCaches = [&] {
            releaseCachedLayers(tiled->createdLayerIds);
            releaseCachedLayers(createdNamespaceLayers);
        };
        if (rasterGpuFrame(surface, submit, displayList, rasterStartNs)) {
            commitTiledState();
            return true;
        }

#if defined(__ANDROID__)
        // Android's canonical retained-layer contract is AHardwareBuffer.
        // Falling through here would hide a broken GPU/native transport behind
        // a full-surface CPU copy and make high-refresh behavior unpredictable.
        discardPreparedCaches();
        return false;
#endif

        // DMA-BUF frames are never silently omitted from a software output.
        if (requiresGpuExternal) {
            discardPreparedCaches();
            return false;
        }

        if (!retainedBaseMatches(
                submit, surface.softwareFrameSerial,
                surface.softwareGeometryGeneration, surface.softwareWidth,
                surface.softwareHeight, surface.softwareScale)) {
            discardPreparedCaches();
            return false;
        }
        LayerSlot* slot = acquireSlot(surface, submit);
        if (!slot) {
            discardPreparedCaches();
            return false;
        }
        const size_t pixelCount =
            static_cast<size_t>(slot->width) * slot->height;
        const bool recreateSoftwareScene = !surface.softwareRenderer ||
            surface.softwareWidth != slot->width ||
            surface.softwareHeight != slot->height;
        if (recreateSoftwareScene) {
            surface.softwareRenderer =
                std::make_unique<lcl::render::RasterRenderer>();
            surface.softwarePixels.assign(pixelCount, 0u);
            if (!surface.softwareRenderer->initialize(
                    slot->width, slot->height, nullptr,
                    surface.softwarePixels.data())) {
                surface.softwareRenderer.reset();
                slot->busy = false;
                discardPreparedCaches();
                return false;
            }
            surface.softwareRenderer->setRetainsFrameBacking(true);
            surface.softwareWidth = slot->width;
            surface.softwareHeight = slot->height;
        }
        auto& renderer = *surface.softwareRenderer;
        renderer.setDeviceScale(submit.bufferScale);
        renderer.setFrameExtent(slot->width, slot->height);
        renderer.beginFrame();
        renderer.setFrameDamageRect(lcl::render::RasterRect{
            submit.damageX, submit.damageY,
            submit.damageWidth, submit.damageHeight});
        const bool replayed = renderer.replayDisplayList(
            displayList,
            {{submit.logicalWidth, submit.logicalHeight},
             {slot->width, slot->height}, submit.bufferScale});
        renderer.endFrame();
        if (!replayed) {
            slot->busy = false;
            discardPreparedCaches();
            return false;
        }
        std::copy(surface.softwarePixels.begin(),
                  surface.softwarePixels.end(), slot->pixels);
        const PixelDamage damage = pixelDamageFor(
            submit, slot->width, slot->height);
        LayerReady ready{};
        ready.grant = submit.grant;
        ready.layerId = slot->layerId;
        ready.configureSerial = submit.configureSerial;
        ready.frameSerial = submit.frameSerial;
        ready.geometryGeneration = submit.geometryGeneration;
        ready.width = slot->width;
        ready.height = slot->height;
        ready.backingWidth = slot->width;
        ready.backingHeight = slot->height;
        ready.stride = slot->width * sizeof(uint32_t);
        ready.damageX = damage.x;
        ready.damageY = damage.y;
        ready.damageWidth = damage.width;
        ready.damageHeight = damage.height;
        ready.transport = lcl::raster_protocol::LayerTransport::Shm;
        ready.byteSize = slot->bytes;
        ready.clientFrameStartNs = submit.clientFrameStartNs;
        ready.clientSubmitNs = submit.clientSubmitNs;
        ready.rasterStartNs = rasterStartNs;
        ready.rasterReadyNs = rasterStartNs != 0 ? monotonicNowNs() : 0;
        if (!lcl::raster_protocol::sendPacket(
                m_compositorFd, Opcode::LayerReady, ready, slot->fd)) {
            slot->busy = false;
            surface.softwareFrameSerial = 0;
            discardPreparedCaches();
            return false;
        }
        surface.softwareFrameSerial = submit.frameSerial;
        surface.softwareGeometryGeneration = submit.geometryGeneration;
        surface.softwareScale = submit.bufferScale;
        commitTiledState();
        return true;
    }

    bool rasterGpuFrame(
            SurfaceState& surface, const CommitTransaction& submit,
            const lcl::graphics::DisplayList& displayList,
            uint64_t rasterStartNs) {
        if (surface.gpuUnavailable) return false;
        const uint32_t width = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalWidth * submit.bufferScale)));
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalHeight * submit.bufferScale)));
        if (!retainedBaseMatches(
                submit, surface.gpuFrameSerial,
                surface.gpuGeometryGeneration, surface.gpuWidth,
                surface.gpuHeight, surface.gpuScale)) {
            return false;
        }
        if (!surface.gpuContext) {
            auto context = std::make_unique<lcl::render::ClientEGLContext>();
            if (!context->initialize(width, height) ||
                !context->hasDmaBufPool()) {
                surface.gpuUnavailable = true;
                return false;
            }
            auto renderer = std::make_unique<lcl::render::RasterRenderer>();
            if (!renderer->initialize(width, height, context.get(), nullptr)) {
                surface.gpuUnavailable = true;
                return false;
            }
            renderer->setRetainsFrameBacking(true);
            surface.gpuContext = std::move(context);
            surface.gpuRenderer = std::move(renderer);
        }
        if (!surface.gpuContext->ensureDmaBufCapacity(width, height) ||
            !surface.gpuRenderer->ensureFrameBackingCapacity(width, height)) {
            return false;
        }
        auto gpuCommands = std::make_shared<std::vector<
            lcl::graphics::DisplayCommand>>(displayList.commands());
        std::vector<uint32_t> importedExternalTextures;
        const auto releaseExternalTextures = [&] {
            for (uint32_t texture : importedExternalTextures) {
                surface.gpuRenderer->releaseDmaBufTexture(texture);
            }
            importedExternalTextures.clear();
        };
        for (auto& command : *gpuCommands) {
            auto* external = std::get_if<
                lcl::graphics::DrawExternalBufferCommand>(&command);
            if (!external || external->bufferId == 0 ||
                external->sampleKind !=
                    lcl::graphics::ExternalBufferSampleKind::Unresolved) {
                continue;
            }
            const auto resource = surface.externalBuffers.find({
                external->bufferId, external->contentRevision});
            if (resource == surface.externalBuffers.end() ||
                resource->second.transport !=
                    lcl::raster_protocol::ExternalBufferTransport::
                        DmaBufArgb8888 ||
                resource->second.bufferFd < 0) {
                releaseExternalTextures();
                return false;
            }
            if (resource->second.acquireFenceFd >= 0) {
                const auto wait = surface.gpuContext->waitNativeFence(
                    resource->second.acquireFenceFd);
                if (wait ==
                        lcl::platform::NativeFenceWaitResult::Unsupported) {
                    releaseExternalTextures();
                    return false;
                }
                resource->second.acquireFenceFd = -1;
                if (wait == lcl::platform::NativeFenceWaitResult::
                                ConsumedFailure) {
                    releaseExternalTextures();
                    return false;
                }
            }
            const lcl::platform::DmaBufDescriptor descriptor{
                resource->second.bufferFd,
                resource->second.width,
                resource->second.height,
                resource->second.stride,
                resource->second.format,
                resource->second.modifier,
            };
            const uint32_t texture = surface.gpuRenderer->importDmaBuf(
                external->bufferId, descriptor);
            if (texture == 0) {
                releaseExternalTextures();
                return false;
            }
            importedExternalTextures.push_back(texture);
            external->resourceKey = texture;
            external->sampleKind =
                lcl::graphics::ExternalBufferSampleKind::GlTexture;
        }
        const auto immutableGpuCommands = std::shared_ptr<const std::vector<
            lcl::graphics::DisplayCommand>>(gpuCommands);
        const lcl::graphics::DisplayList gpuDisplayList(
            immutableGpuCommands);
        const auto target = surface.gpuContext->acquireDmaBufTarget();
        if (!target) {
            releaseExternalTextures();
            return false;
        }
        const lcl::platform::RetainedOutputDamageTracker::Frame outputFrame{
            submit.frameSerial,
            submit.baseFrameSerial,
            submit.geometryGeneration,
            width,
            height,
            submit.bufferScale,
            lcl::platform::PresentationDamage{
                submit.damageX, submit.damageY,
                submit.damageWidth, submit.damageHeight},
            replacesRetainedScene(submit),
        };
        std::optional<lcl::render::RasterRect> rasterOutputDamage;
#if defined(__ANDROID__)
        const auto outputDamage = surface.gpuOutputDamage.copyDamage(
            target->bufferId, outputFrame);
        if (outputDamage) {
            rasterOutputDamage = {
                outputDamage->x, outputDamage->y,
                outputDamage->width, outputDamage->height};
        }
#else
        // The desktop GBM path has no release-fence handoff to establish the
        // exact retained revision in a reused output buffer. The scene FBO is
        // authoritative, so copy it completely rather than exposing stale
        // tiles from a prior producer frame. This is one GPU texture copy,
        // not a full Widget/DisplayList repaint.
#endif
        surface.gpuRenderer->setExternalFrameTarget(
            target->framebuffer, target->texture,
            target->width, target->height);
        surface.gpuRenderer->setDeviceScale(submit.bufferScale);
        surface.gpuRenderer->setFrameExtent(width, height);
        surface.gpuRenderer->beginFrame();
        surface.gpuRenderer->setFrameDamageRect(lcl::render::RasterRect{
            submit.damageX, submit.damageY,
            submit.damageWidth, submit.damageHeight});
        const bool replayed = surface.gpuRenderer->replayDisplayList(
            gpuDisplayList,
            {{submit.logicalWidth, submit.logicalHeight},
             {width, height}, submit.bufferScale});
        surface.gpuRenderer->setOutputFrameDamageRect(rasterOutputDamage);
        surface.gpuRenderer->endFrame();
        releaseExternalTextures();
        if (!replayed) {
            surface.gpuRenderer->clearExternalFrameTarget();
            surface.gpuContext->cancelCurrentDmaBuf();
            surface.gpuOutputDamage.invalidateBuffer(target->bufferId);
            return false;
        }
        const auto exported = surface.gpuContext->exportCurrentDmaBuf();
        surface.gpuRenderer->clearExternalFrameTarget();
#if defined(__ANDROID__)
        const bool invalidExport = !exported ||
            !exported->androidHardwareBuffer || exported->fd >= 0 ||
            exported->format == 0;
#else
        const bool invalidExport = !exported || exported->fd < 0 ||
            exported->androidHardwareBuffer;
#endif
        if (invalidExport) {
            surface.gpuContext->releaseDmaBuf(target->bufferId);
            surface.gpuOutputDamage.invalidateBuffer(target->bufferId);
            return false;
        }
        const uint64_t layerId =
            (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
            m_nextLayerId++;
        auto [bufferIdentity, inserted] = surface.gpuBufferIds.try_emplace(
            target->bufferId, 0);
        if (inserted) {
            bufferIdentity->second =
                (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
                m_nextBufferId++;
        }
        LayerReady ready{};
        ready.grant = submit.grant;
        ready.layerId = layerId;
        ready.bufferId = bufferIdentity->second;
        ready.configureSerial = submit.configureSerial;
        ready.frameSerial = submit.frameSerial;
        ready.geometryGeneration = submit.geometryGeneration;
        ready.width = width;
        ready.height = height;
        ready.backingWidth = exported->width;
        ready.backingHeight = exported->height;
        ready.stride = exported->stride;
        const PixelDamage damage = pixelDamageFor(submit, width, height);
        ready.damageX = damage.x;
        ready.damageY = damage.y;
        ready.damageWidth = damage.width;
        ready.damageHeight = damage.height;
        ready.transport =
#if defined(__ANDROID__)
            lcl::raster_protocol::LayerTransport::AndroidHardwareBuffer;
#else
            lcl::raster_protocol::LayerTransport::DmaBuf;
#endif
        ready.format = exported->format;
        ready.modifier = exported->modifier;
        ready.clientFrameStartNs = submit.clientFrameStartNs;
        ready.clientSubmitNs = submit.clientSubmitNs;
        ready.rasterStartNs = rasterStartNs;
        ready.rasterReadyNs = rasterStartNs != 0 ? monotonicNowNs() : 0;
        const int layerDescriptor =
#if defined(__ANDROID__)
            exported->acquireFenceFd;
#else
            exported->fd;
#endif
#if defined(__ANDROID__)
        // Queue the native handle first so LayerReady is never visible before
        // its sideband storage. Any failure terminates both private channels,
        // preventing an orphan handle from being paired with a later frame.
        if (!surface.gpuContext->sendNativeBufferHandle(
                m_nativeBufferFd, target->bufferId)) {
            if (layerDescriptor >= 0) close(layerDescriptor);
            surface.gpuContext->releaseDmaBuf(target->bufferId);
            surface.gpuOutputDamage.reset();
            surface.gpuFrameSerial = 0;
            m_running = false;
            return false;
        }
#endif
        const bool sent = lcl::raster_protocol::sendPacket(
            m_compositorFd, Opcode::LayerReady, ready, layerDescriptor);
        if (layerDescriptor >= 0) close(layerDescriptor);
        if (!sent) {
            std::cerr << "[LCL Rasterd] LayerReady publish failed"
                      << " (layer=" << layerId << ")\n";
            surface.gpuContext->releaseDmaBuf(target->bufferId);
            surface.gpuOutputDamage.reset();
            surface.gpuFrameSerial = 0;
#if defined(__ANDROID__)
            m_running = false;
#endif
            return false;
        }
        surface.gpuOutputDamage.commit(target->bufferId, outputFrame);
        surface.gpuLayers.emplace(layerId, target->bufferId);
        surface.gpuFrameSerial = submit.frameSerial;
        surface.gpuGeometryGeneration = submit.geometryGeneration;
        surface.gpuWidth = width;
        surface.gpuHeight = height;
        surface.gpuScale = submit.bufferScale;
        return true;
    }

    void discard(int clientFd, const CommitTransaction& submit,
                 lcl::raster_protocol::DiscardReason reason) {
        FrameDiscarded discarded{};
        discarded.surfaceId = submit.grant.surfaceId;
        discarded.configureSerial = submit.configureSerial;
        discarded.frameSerial = submit.frameSerial;
        discarded.geometryGeneration = submit.geometryGeneration;
        discarded.reason = reason;
        (void)lcl::raster_protocol::sendPacket(
            clientFd, Opcode::FrameDiscarded, discarded);
    }

    void releaseLayer(
            uint64_t layerId,
            lcl::raster_protocol::LayerReleaseReason reason,
            int releaseFenceFd) {
        for (auto& [_, surface] : m_surfaces) {
            const auto gpu = surface.gpuLayers.find(layerId);
            if (gpu != surface.gpuLayers.end()) {
                if (surface.gpuContext) {
                    surface.gpuContext->releaseDmaBuf(
                        gpu->second, releaseFenceFd);
                    releaseFenceFd = -1;
                }
                surface.gpuLayers.erase(gpu);
                if (reason == lcl::raster_protocol::LayerReleaseReason::RejectedTransport) {
                    // The compositor could not import this producer's native
                    // GPU transport. Keep already-retained layers alive and
                    // stop retrying this unusable path for the surface.
                    surface.gpuUnavailable = true;
                }
                if (releaseFenceFd >= 0) close(releaseFenceFd);
                return;
            }
            for (auto& slot : surface.slots) {
                if (slot.layerId == layerId) {
                    slot.busy = false;
                    if (releaseFenceFd >= 0) close(releaseFenceFd);
                    return;
                }
            }
        }
        if (releaseFenceFd >= 0) close(releaseFenceFd);
    }

    void notifyExternalBufferRelease(
            SurfaceState& surface, const ExternalKey& key,
            ExternalBufferResource& resource,
            lcl::raster_protocol::ExternalBufferReleaseReason reason) {
        int releaseFenceFd = -1;
        if (resource.transport ==
                lcl::raster_protocol::ExternalBufferTransport::
                    DmaBufArgb8888 &&
            surface.gpuContext && surface.gpuRenderer) {
            releaseFenceFd = surface.gpuContext->createNativeFence();
            surface.gpuRenderer->discardDmaBufBuffer(key.id);
        }
        lcl::raster_protocol::ExternalBufferReleased released{};
        released.bufferId = key.id;
        released.contentRevision = key.revision;
        released.reason = reason;
        (void)lcl::raster_protocol::sendPacket(
            resource.clientFd, Opcode::ExternalBufferReleased, released,
            releaseFenceFd);
        if (releaseFenceFd >= 0) close(releaseFenceFd);
    }

    void pruneExternalBuffers(SurfaceState& surface) {
        std::unordered_set<ExternalKey, ExternalHash> active;
        for (const auto& [_, node] : surface.retainedNodes) {
            if ((node.flags &
                 lcl::raster_protocol::kNodeHasExternalBuffer) != 0) {
                active.insert({node.externalBufferId,
                               node.externalBufferRevision});
            }
        }
        for (auto resource = surface.externalBuffers.begin();
             resource != surface.externalBuffers.end();) {
            if (active.contains(resource->first)) {
                ++resource;
                continue;
            }
            notifyExternalBufferRelease(
                surface, resource->first, resource->second,
                lcl::raster_protocol::ExternalBufferReleaseReason::
                    Superseded);
            resource = surface.externalBuffers.erase(resource);
        }
    }

    void revokeSurface(const TokenKey& token) {
        const auto found = m_surfaces.find(token);
        if (found == m_surfaces.end()) return;
        const auto removePending = [&token](auto& queue) {
            for (auto it = queue.begin(); it != queue.end();) {
                if (tokenOf(it->transaction.grant) == token) {
                    if (it->displayListFd >= 0) close(it->displayListFd);
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        };
        removePending(m_systemFrames);
        removePending(m_appFrames);
        for (auto& [key, resource] : found->second.externalBuffers) {
            notifyExternalBufferRelease(
                found->second, key, resource,
                lcl::raster_protocol::ExternalBufferReleaseReason::
                    SurfaceRevoked);
        }
        m_surfaces.erase(found);
    }

    int m_compositorFd{-1};
    int m_nativeBufferFd{-1};
    std::string m_socketPath;
    int m_listenerFd{-1};
    bool m_running{true};
    std::vector<Client> m_clients;
    std::unordered_map<TokenKey, SurfaceState, TokenHash> m_surfaces;
    std::deque<PendingFrame> m_systemFrames;
    std::deque<PendingFrame> m_appFrames;
    unsigned m_systemBurst{0};
    uint64_t m_nextLayerId{1};
    uint32_t m_nextBufferId{1};
    uint64_t m_nextImageId{1};
    uint64_t m_nextCachedLayerId{1};
};

std::optional<int> parseFd(const char* value) {
    if (!value || !*value) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > 1024) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    int compositorFd = -1;
    int nativeBufferFd = -1;
    std::string socketPath = "/Runtime/lcl-raster.sock";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--compositor-fd" && index + 1 < argc) {
            compositorFd = parseFd(argv[++index]).value_or(-1);
        } else if (argument == "--native-buffer-fd" && index + 1 < argc) {
            nativeBufferFd = parseFd(argv[++index]).value_or(-1);
        } else if (argument == "--socket" && index + 1 < argc) {
            socketPath = argv[++index];
        }
    }
    RasterService service(
        compositorFd, nativeBufferFd, std::move(socketPath));
    if (!service.initialize()) {
        std::cerr << "[LCL Rasterd ERROR] initialization failed\n";
        return 1;
    }
    std::cout << "[LCL Rasterd] retained layer producer ready\n";
    return service.run();
}
