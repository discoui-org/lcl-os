#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"

namespace lcl::core {

/**
 * Owns all compositor-side client surface resources.
 *
 * A registry entry owns precisely one optional mapped SHM buffer and its memfd.
 * Erasing an entry (or destroying the registry) always releases both resources,
 * so disconnect, close animation completion and shutdown share one cleanup path.
 */
class SurfaceRegistry {
public:
    struct SurfaceEffectRegion {
        protocol::EffectRegion region{};
        std::vector<protocol::FilterOp> filters;
        bool followSurfaceBounds{false};
    };

    struct SurfaceEntry {
        enum class TransitionPhase {
            None,
            Entering,
            Closing,
        };

        uint32_t windowId{0};
        int clientFd{-1};
        int shmFd{-1};
        void* pixels{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        float bufferScale{1.0f};
        size_t shmSize{0};
        std::string appId;
        std::vector<SurfaceEffectRegion> effectRegions;

        int configuredX{0};
        int configuredY{0};
        uint32_t configuredWidth{0};
        uint32_t configuredHeight{0};
        uint8_t configuredFocused{0};
        std::chrono::steady_clock::time_point lastConfigureSent{};

        TransitionPhase transitionPhase{TransitionPhase::None};
        float transitionElapsedSec{0.0f};
        float transitionDurationSec{0.0f};
        float transitionOpacity{1.0f};
        float transitionScale{1.0f};
        bool hasCommittedBuffer{false};
        bool ignoreBufferCommits{false};
        bool pendingDestroy{false};
    };

    using Key = uint64_t;
    using Entries = std::unordered_map<Key, SurfaceEntry>;
    using iterator = Entries::iterator;
    using const_iterator = Entries::const_iterator;

    /** A non-owning, read-only view stable for a single compositor frame. */
    struct SnapshotEntry {
        Key key{0};
        const SurfaceEntry* entry{nullptr};
    };
    using Snapshot = std::vector<SnapshotEntry>;

    SurfaceRegistry() = default;
    ~SurfaceRegistry();

    SurfaceRegistry(const SurfaceRegistry&) = delete;
    SurfaceRegistry& operator=(const SurfaceRegistry&) = delete;

    static Key makeKey(int clientFd, pid_t pid, uint32_t surfaceId) noexcept;

    iterator begin() noexcept { return m_entries.begin(); }
    iterator end() noexcept { return m_entries.end(); }
    const_iterator begin() const noexcept { return m_entries.begin(); }
    const_iterator end() const noexcept { return m_entries.end(); }
    const_iterator cbegin() const noexcept { return m_entries.cbegin(); }
    const_iterator cend() const noexcept { return m_entries.cend(); }

    iterator find(Key key) { return m_entries.find(key); }
    const_iterator find(Key key) const { return m_entries.find(key); }
    SurfaceEntry& operator[](Key key) { return m_entries[key]; }

    bool contains(Key key) const { return m_entries.contains(key); }
    size_t size() const noexcept { return m_entries.size(); }
    bool empty() const noexcept { return m_entries.empty(); }

    Snapshot snapshot() const;

    iterator erase(iterator position);
    size_t erase(Key key);
    void clear() noexcept;

    /** Release an entry's mapped SHM and memfd without erasing its metadata. */
    static void releaseBuffer(SurfaceEntry& entry) noexcept;

private:
    Entries m_entries;
};

} // namespace lcl::core
