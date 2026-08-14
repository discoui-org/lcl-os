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

bool SurfaceRegistry::isOwnedByClientConnection(const SurfaceEntry& entry,
                                                int clientFd) noexcept {
    return clientFd >= 0 && entry.clientFd == clientFd;
}

} // namespace lcl::core
