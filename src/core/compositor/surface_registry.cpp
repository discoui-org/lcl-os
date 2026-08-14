#include "core/compositor/surface_registry.hpp"

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

SurfaceRegistry::Snapshot SurfaceRegistry::snapshot() const {
    Snapshot result;
    result.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        result.push_back(SnapshotEntry{key, &entry});
    }
    return result;
}

SurfaceRegistry::iterator SurfaceRegistry::erase(iterator position) {
    if (position != m_entries.end()) {
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
    for (auto& [_, entry] : m_entries) {
        releaseBuffer(entry);
    }
    m_entries.clear();
}

void SurfaceRegistry::releaseBuffer(SurfaceEntry& entry) noexcept {
    completeLivePresentation(entry);
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
    if (entry.previousShmFd >= 0) close(entry.previousShmFd);
    entry.previousShmFd = -1;
    entry.previousWidth = entry.previousHeight = entry.previousStride = 0;
    entry.previousBackingWidth = entry.previousBackingHeight = 0;
}

void SurfaceRegistry::interruptGeometryTransaction(SurfaceEntry& entry,
                                                   uint64_t newGeneration) noexcept {
    const bool preserveLiveFlight =
        entry.resizePresentation == protocol::LCLResizePresentationMode::Live &&
        (hasOutstandingConfigure(entry) || hasUnpresentedLiveFrame(entry));
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
                                              int rollbackX, int rollbackY,
                                              int rollbackWidth, int rollbackHeight,
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

bool SurfaceRegistry::hasOutstandingConfigure(const SurfaceEntry& entry) noexcept {
    return entry.pendingConfigureSerial != 0 &&
           entry.pendingConfigureSerial != entry.acceptedConfigureSerial;
}

bool SurfaceRegistry::hasUnpresentedLiveFrame(const SurfaceEntry& entry) noexcept {
    return entry.resizePresentation == protocol::LCLResizePresentationMode::Live &&
           entry.livePresentationSerial != 0;
}

void SurfaceRegistry::queueLivePresentation(SurfaceEntry& entry,
                                            uint64_t configureSerial) noexcept {
    if (entry.resizePresentation == protocol::LCLResizePresentationMode::Live) {
        entry.livePresentationSerial = configureSerial;
    }
}

void SurfaceRegistry::completeLivePresentation(SurfaceEntry& entry) noexcept {
    entry.livePresentationSerial = 0;
}

bool SurfaceRegistry::isOwnedByClientConnection(const SurfaceEntry& entry,
                                                int clientFd) noexcept {
    return clientFd >= 0 && entry.clientFd == clientFd;
}

} // namespace lcl::core
