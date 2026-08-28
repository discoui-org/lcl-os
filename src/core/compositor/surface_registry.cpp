#include "core/compositor/surface_registry.hpp"
#include "core/compositor/surface_transaction_coordinator.hpp"
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
    // current retained raster layer can be animated.
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
            SurfaceTransactionCoordinator::cancel(
                *this, windowId, atomicGeneration);
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
    if (entry.pixels && entry.shmSize > 0) {
        munmap(entry.pixels, entry.shmSize);
    }
    entry.pixels = nullptr;
    entry.shmSize = 0;
    entry.shmContentSerial = 0;
    entry.shmDamageX = entry.shmDamageY = 0;
    entry.shmDamageWidth = entry.shmDamageHeight = 0;

    if (entry.shmFd >= 0) {
        close(entry.shmFd);
    }
    entry.shmFd = -1;
}

void SurfaceRegistry::interruptGeometryTransaction(SurfaceEntry& entry,
                                                   uint64_t newGeneration) noexcept {
    const bool preserveLiveFlight =
        hasOutstandingConfigure(entry) || hasUnpresentedFrame(entry);

    // Rasterd may already be producing the only in-flight serial. Keep it valid
    // until accepted/presented; generation validation rejects obsolete geometry.
    // The forced configure below then publishes only the newest target.
    if (!preserveLiveFlight) {
        entry.pendingConfigureSerial = 0;
        entry.configuredGeometryGeneration = newGeneration;
    }
    entry.forceConfigure = true;
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

void SurfaceRegistry::queuePresentation(SurfaceEntry& entry,
                                        uint64_t configureSerial) noexcept {
    entry.presentationSerial = configureSerial;
}

void SurfaceRegistry::completePresentation(SurfaceEntry& entry) noexcept {
    entry.presentationSerial = 0;
}

bool SurfaceRegistry::isOwnedByClientConnection(const SurfaceEntry& entry,
                                                int clientFd) noexcept {
    return clientFd >= 0 && entry.clientFd == clientFd;
}

} // namespace lcl::core
