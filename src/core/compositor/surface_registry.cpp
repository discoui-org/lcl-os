#include "core/compositor/surface_registry.hpp"
#include "lcl-motion/motion.hpp"

#include <algorithm>
#include <cmath>
#include <sys/mman.h>
#include <unistd.h>

namespace lcl::core {

SurfaceRegistry::~SurfaceRegistry() {
    clear();
}

SurfaceRegistry::Key SurfaceRegistry::makeKey(int clientFd, pid_t pid, uint32_t surfaceId) noexcept {
    const auto owner = static_cast<uint64_t>(pid > 0 ? pid : clientFd);
    return (owner << 32) | static_cast<uint64_t>(surfaceId);
}

bool SurfaceRegistry::beginClosingTransition(SurfaceEntry& entry) noexcept {
    // Stop accepting replacement frames before deciding whether the frozen
    // current frame can be animated. Both SHM and DMA-BUF are valid sources.
    entry.ignoreBufferCommits = true;
    if (entry.suppressInitialTransition || entry.windowId == 0 ||
        !entry.hasRenderableBuffer() || entry.width == 0 || entry.height == 0) {
        return false;
    }

    entry.transitionPhase = SurfaceEntry::TransitionPhase::Closing;
    entry.transitionElapsedSec = 0.0f;
    entry.transitionDurationSec = lcl::motion::tokens::windowClose().tweenParams.durationSec;
    entry.transitionOpacity = 1.0f;
    entry.transitionScale = 1.0f;
    entry.pendingDestroy = false;
    return true;
}

SurfaceRegistry::Snapshot SurfaceRegistry::snapshot() const {
    Snapshot result;
    result.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        result.push_back(SnapshotEntry{key, &entry});
    }
    return result;
}

std::vector<SurfaceRegistry::Key> SurfaceRegistry::popupChildren(
        Key parentSurfaceKey) const {
    std::vector<Key> result;
    for (const auto& [key, entry] : m_entries) {
        if (entry.parentSurfaceKey == parentSurfaceKey) result.push_back(key);
    }
    std::sort(result.begin(), result.end(), [this](Key lhs, Key rhs) {
        return m_entries.at(lhs).popupOrder < m_entries.at(rhs).popupOrder;
    });
    return result;
}

std::vector<SurfaceRegistry::Key> SurfaceRegistry::attachedChildren(
        uint32_t windowId) const {
    std::vector<Key> result;
    for (const auto& [key, entry] : m_entries) {
        if (entry.attachedWindowId == windowId) result.push_back(key);
    }
    std::sort(result.begin(), result.end(), [this](Key lhs, Key rhs) {
        const auto& left = m_entries.at(lhs);
        const auto& right = m_entries.at(rhs);
        if (left.attachedRole != right.attachedRole) {
            return static_cast<uint32_t>(left.attachedRole) <
                static_cast<uint32_t>(right.attachedRole);
        }
        return left.attachmentOrder < right.attachmentOrder;
    });
    return result;
}

bool SurfaceRegistry::focusKeyboardSurface(Key key) noexcept {
    if (key != 0 && !m_entries.contains(key)) return false;
    m_keyboardFocusSurface = key;
    return true;
}

void SurfaceRegistry::releaseKeyboardFocus(Key key) noexcept {
    if (key == 0 || m_keyboardFocusSurface == 0) return;

    const auto focused = m_entries.find(m_keyboardFocusSurface);
    if (m_keyboardFocusSurface != key) {
        if (focused != m_entries.end() && focused->second.parentSurfaceKey == key) {
            m_keyboardFocusSurface = 0;
        }
        return;
    }

    const Key parentKey = focused == m_entries.end()
        ? 0
        : focused->second.parentSurfaceKey;
    const auto parent = m_entries.find(parentKey);
    m_keyboardFocusSurface = parentKey != 0 && parent != m_entries.end() &&
            !parent->second.pendingDestroy && !parent->second.ignoreBufferCommits
        ? parentKey
        : 0;
}

SurfaceRegistry::iterator SurfaceRegistry::erase(iterator position) {
    if (position != m_entries.end()) {
        const uint32_t windowId = position->second.windowId;
        const uint64_t atomicGeneration =
            position->second.atomicConfigureGeneration;
        if (atomicGeneration != 0) {
            cancelAtomicConfigure(windowId, atomicGeneration);
        }
        releaseKeyboardFocus(position->first);
        releaseBuffer(position->second);
    }
    return m_entries.erase(position);
}

size_t SurfaceRegistry::erase(Key key) {
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        return 0;
    }
    erase(found);
    return 1;
}

void SurfaceRegistry::clear() noexcept {
    m_keyboardFocusSurface = 0;
    for (auto& [_, entry] : m_entries) {
        releaseBuffer(entry);
    }
    m_entries.clear();
}

void SurfaceRegistry::releaseBuffer(SurfaceEntry& entry) noexcept {
    completePresentation(entry);
    releasePreviousBuffer(entry);
    if (entry.dmaBufTexture != 0 || entry.dmaBufId != 0) {
        entry.pendingDmaBufReleases.push_back({entry.dmaBufId, entry.dmaBufTexture});
    }
    entry.dmaBufId = 0;
    entry.dmaBufTexture = 0;
    if (entry.pixels && entry.shmSize > 0) {
        munmap(entry.pixels, entry.shmSize);
    }
    entry.pixels = nullptr;
    entry.shmSize = 0;
    entry.shmContentSerial = 0;
    entry.shmDamageX = entry.shmDamageY = 0;
    entry.shmDamageWidth = entry.shmDamageHeight = 0;
    entry.displayList = {};
    entry.displayListSerial = 0;
    entry.displayListCacheId = 0;
    entry.displayListWidth = entry.displayListHeight = 0.0f;
    entry.imageResources.clear();
    entry.imageResourceBytes = 0;
    entry.cachedLayerNamespaces.clear();

    if (entry.shmFd >= 0) {
        close(entry.shmFd);
    }
    entry.shmFd = -1;
}

void SurfaceRegistry::releasePreviousBuffer(SurfaceEntry& entry) noexcept {
    if (entry.previousDmaBufTexture != 0 || entry.previousDmaBufId != 0) {
        entry.pendingDmaBufReleases.push_back({entry.previousDmaBufId, entry.previousDmaBufTexture});
    }
    entry.previousDmaBufId = 0;
    entry.previousDmaBufTexture = 0;
    if (entry.previousPixels && entry.previousShmSize > 0) {
        munmap(entry.previousPixels, entry.previousShmSize);
    }
    entry.previousPixels = nullptr;
    entry.previousShmSize = 0;
    entry.previousShmContentSerial = 0;
    entry.previousDisplayList = {};
    entry.previousDisplayListSerial = 0;
    entry.previousDisplayListCacheId = 0;
    entry.previousDisplayListWidth = entry.previousDisplayListHeight = 0.0f;
    if (entry.previousShmFd >= 0) close(entry.previousShmFd);
    entry.previousShmFd = -1;
    entry.previousWidth = entry.previousHeight = entry.previousStride = 0;
    entry.previousBackingWidth = entry.previousBackingHeight = 0;
}

void SurfaceRegistry::interruptGeometryTransaction(SurfaceEntry& entry,
                                                   uint64_t newGeneration) noexcept {
    const bool preserveLiveFlight =
        entry.resizePresentation == protocol::LCLResizePresentationMode::Live &&
        (hasOutstandingConfigure(entry) || hasUnpresentedFrame(entry));
    releasePreviousBuffer(entry);
    entry.resizeTransitionPhase = SurfaceEntry::ResizeTransitionPhase::None;
    entry.resizeCrossfadeElapsedSec = 0.0f;
    entry.resizeCrossfadeProgress = 1.0f;
    entry.resizeBufferReady = true;
    entry.rollbackRequested = false;
    entry.resizeGeometryGeneration = 0;

    // A Live client may already be rendering the only in-flight serial. Keep
    // that serial valid until its real buffer is accepted and presented; the
    // generation check prevents it from committing obsolete window geometry.
    // The forced configure below then publishes only the newest target. Morph
    // transactions retain their existing cancellation semantics.
    if (!preserveLiveFlight) {
        entry.pendingConfigureSerial = 0;
        entry.configuredGeometryGeneration = newGeneration;
    }
    entry.forceConfigure = true;
}

void SurfaceRegistry::beginGeometryTransition(SurfaceEntry& entry,
                                              uint64_t newGeneration,
                                              float rollbackX, float rollbackY,
                                              float rollbackWidth, float rollbackHeight,
                                              bool rollbackWasMaximized,
                                              bool rollbackWasMinimized) noexcept {
    interruptGeometryTransaction(entry, newGeneration);
    entry.rollbackX = rollbackX;
    entry.rollbackY = rollbackY;
    entry.rollbackWidth = rollbackWidth;
    entry.rollbackHeight = rollbackHeight;
    entry.rollbackWasMaximized = rollbackWasMaximized;
    entry.rollbackWasMinimized = rollbackWasMinimized;
    entry.resizeTransitionPhase = SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer;
    entry.resizeGeometryGeneration = newGeneration;
    entry.resizeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    entry.resizeCrossfadeElapsedSec = 0.0f;
    entry.resizeCrossfadeProgress = 0.0f;
    entry.resizeBufferReady = false;
    entry.rollbackRequested = false;
}

bool SurfaceRegistry::acceptsBufferCommit(const SurfaceEntry& entry,
                                          uint64_t configureSerial) noexcept {
    // The serial identifies the configure generation. Buffer dimensions are
    // deliberately not part of freshness validation: clients such as Terminal
    // may constrain a requested resize to their cell grid and reply with the
    // closest valid size for that same configure. WindowManager reconciles the
    // accepted dimensions through commitSurfaceGeometry().
    return configureSerial != 0 && configureSerial == entry.pendingConfigureSerial;
}

float SurfaceRegistry::committedLogicalExtent(uint32_t physicalExtent,
                                              float requestedLogicalExtent,
                                              float bufferScale) noexcept {
    const float scale = std::isfinite(bufferScale) && bufferScale > 0.0f
        ? bufferScale : 1.0f;
    if (std::isfinite(requestedLogicalExtent) && requestedLogicalExtent > 0.0f) {
        const uint32_t requestedPhysical = static_cast<uint32_t>(std::ceil(
            requestedLogicalExtent * scale));
        if (physicalExtent == requestedPhysical) {
            // Preserve the exact fractional logical configure when the client
            // accepted it verbatim; ceil(logical * scale) is not reversible.
            return requestedLogicalExtent;
        }
    }
    return static_cast<float>(physicalExtent) / scale;
}

bool SurfaceRegistry::hasOutstandingConfigure(const SurfaceEntry& entry) noexcept {
    return entry.pendingConfigureSerial != 0 &&
           entry.pendingConfigureSerial != entry.acceptedConfigureSerial;
}

bool SurfaceRegistry::hasUnpresentedFrame(const SurfaceEntry& entry) noexcept {
    return entry.presentationSerial != 0;
}

bool SurfaceRegistry::beginAtomicConfigure(
        uint32_t windowId, uint64_t generation,
        const std::vector<Key>& participants,
        std::chrono::steady_clock::time_point deadline) noexcept {
    if (windowId == 0 || generation == 0 || participants.size() < 2) {
        return false;
    }
    for (const auto key : participants) {
        const auto found = m_entries.find(key);
        if (found == m_entries.end() || found->second.windowId != windowId) {
            return false;
        }
    }
    const auto participantCount = static_cast<uint32_t>(participants.size());
    for (const auto key : participants) {
        auto& entry = m_entries.at(key);
        entry.atomicConfigureGeneration = generation;
        entry.atomicConfigureParticipantCount = participantCount;
        entry.atomicConfigureIssued = key == participants.front();
        entry.atomicConfigureDeadline = deadline;
    }
    return true;
}

void SurfaceRegistry::cancelAtomicConfigure(
        uint32_t windowId, uint64_t generation) noexcept {
    for (auto& [_, entry] : m_entries) {
        if (entry.windowId != windowId ||
            entry.atomicConfigureGeneration != generation) continue;
        entry.atomicConfigureGeneration = 0;
        entry.atomicConfigureParticipantCount = 0;
        entry.atomicConfigureIssued = false;
        entry.atomicConfigureDeadline = {};
    }
}

bool SurfaceRegistry::hasIncompleteAtomicConfigure(
        std::chrono::steady_clock::time_point now) noexcept {
    struct GroupState {
        uint32_t windowId{0};
        uint64_t generation{0};
        uint32_t expected{0};
        uint32_t seen{0};
        bool ready{true};
        bool expired{false};
    };
    std::vector<GroupState> groups;
    for (const auto& [_, entry] : m_entries) {
        if (entry.atomicConfigureGeneration == 0) continue;
        auto group = std::find_if(
            groups.begin(), groups.end(), [&entry](const GroupState& item) {
                return item.windowId == entry.windowId &&
                    item.generation == entry.atomicConfigureGeneration;
            });
        if (group == groups.end()) {
            groups.push_back({entry.windowId,
                              entry.atomicConfigureGeneration,
                              entry.atomicConfigureParticipantCount});
            group = groups.end() - 1;
        }
        ++group->seen;
        group->expected = std::max(
            group->expected, entry.atomicConfigureParticipantCount);
        group->ready = group->ready &&
            entry.atomicConfigureIssued &&
            entry.configuredGeometryGeneration == group->generation &&
            entry.acceptedConfigureSerial == entry.pendingConfigureSerial &&
            entry.presentationSerial == entry.pendingConfigureSerial;
        group->expired = group->expired ||
            (entry.atomicConfigureDeadline.time_since_epoch().count() != 0 &&
             now >= entry.atomicConfigureDeadline);
    }

    bool incomplete = false;
    for (const auto& group : groups) {
        const bool ready = group.ready && group.seen == group.expected;
        if (!ready && !group.expired) {
            incomplete = true;
            continue;
        }
        cancelAtomicConfigure(group.windowId, group.generation);
    }
    return incomplete;
}

void SurfaceRegistry::queuePresentation(SurfaceEntry& entry,
                                        uint64_t configureSerial) noexcept {
    entry.presentationSerial = configureSerial;
}

void SurfaceRegistry::completePresentation(SurfaceEntry& entry) noexcept {
    entry.presentationSerial = 0;
}

void SurfaceRegistry::pruneUnreferencedImageResources(
        SurfaceEntry& entry, size_t targetCount,
        size_t targetBytes) noexcept {
    const auto isReferenced = [&entry](
            const SurfaceEntry::ImageResource& resource) {
        const uintptr_t pixels = reinterpret_cast<uintptr_t>(
            resource.pixels.data());
        const auto listReferences = [pixels](const graphics::DisplayList& list) {
            return std::any_of(
                list.commands().begin(), list.commands().end(),
                [pixels](const graphics::DisplayCommand& command) {
                    const auto* image = std::get_if<graphics::DrawImageCommand>(
                        &command);
                    return image && image->resourceKey == pixels;
                });
        };
        return listReferences(entry.displayList) ||
            listReferences(entry.previousDisplayList);
    };

    for (auto resource = entry.imageResources.begin();
         resource != entry.imageResources.end() &&
             (entry.imageResources.size() > targetCount ||
              entry.imageResourceBytes > targetBytes);) {
        if (isReferenced(resource->second)) {
            ++resource;
            continue;
        }
        const size_t bytes = resource->second.pixels.size() * sizeof(uint32_t);
        entry.imageResourceBytes = bytes > entry.imageResourceBytes
            ? 0 : entry.imageResourceBytes - bytes;
        resource = entry.imageResources.erase(resource);
    }
}

bool SurfaceRegistry::isOwnedByClientConnection(const SurfaceEntry& entry,
                                                int clientFd) noexcept {
    return clientFd >= 0 && entry.clientFd == clientFd;
}

} // namespace lcl::core
