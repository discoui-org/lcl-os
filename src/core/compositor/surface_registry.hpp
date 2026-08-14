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
            Minimizing,
            Restoring,
        };

        enum class ResizeTransitionPhase { None, AwaitingBuffer, Crossfading };

        uint32_t windowId{0};
        int clientFd{-1};
        int shmFd{-1};
        void* pixels{nullptr};
        uint32_t dmaBufId{0};
        uint32_t dmaBufTexture{0};
        bool dmaBufTransportActive{false};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t backingWidth{0};
        uint32_t backingHeight{0};
        uint32_t stride{0};
        float bufferScale{1.0f};
        size_t shmSize{0};
        // A surface is registered before it is mapped.  Keep its window policy
        // here until the first complete client buffer is ready to present.
        std::string title;
        int initialX{0};
        int initialY{0};
        uint32_t initialWidth{0};
        uint32_t initialHeight{0};
        protocol::LCLSystemSurfaceKind systemSurfaceKind{protocol::LCLSystemSurfaceKind::None};
        protocol::LCLDecorationMode decorationMode{protocol::LCLDecorationMode::SSD};
        bool edgeToEdge{false};
        protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};
        bool unfocusable{false};
        bool insetBorderEnabled{true};
        protocol::LCLResizePresentationMode resizePresentation{
            protocol::LCLResizePresentationMode::CompositorMorph};
        float cornerRadiusPx{-1.0f};
        float cornerRoundness{2.0f};
        bool suppressInitialTransition{false};
        std::string appId;
        std::vector<SurfaceEffectRegion> effectRegions;

        int configuredX{0};
        int configuredY{0};
        uint32_t configuredWidth{0};
        uint32_t configuredHeight{0};
        uint8_t configuredFocused{0};
        uint64_t nextConfigureSerial{1};
        uint64_t pendingConfigureSerial{0};
        uint64_t acceptedConfigureSerial{0};
        uint64_t configuredGeometryGeneration{0};
        // Non-zero only after a Live DMA-BUF commit has been accepted and
        // before the compositor frame containing it has been presented.
        // Keeping this separate from configure acknowledgement gives Live
        // surfaces one complete configure -> commit -> presentation in flight.
        uint64_t livePresentationSerial{0};
        bool forceConfigure{false};
        std::chrono::steady_clock::time_point lastConfigureSent{};

        TransitionPhase transitionPhase{TransitionPhase::None};
        float transitionElapsedSec{0.0f};
        float transitionDurationSec{0.0f};
        float transitionOpacity{1.0f};
        float transitionScale{1.0f};
        bool hasCommittedBuffer{false};
        bool ignoreBufferCommits{false};
        bool pendingDestroy{false};
        bool pendingMinimize{false};

        // During compositor geometry morphs the last accepted client frame is
        // retained until the matching configure serial arrives.
        int previousShmFd{-1};
        void* previousPixels{nullptr};
        uint32_t previousDmaBufId{0};
        uint32_t previousDmaBufTexture{0};
        uint32_t previousWidth{0};
        uint32_t previousHeight{0};
        uint32_t previousBackingWidth{0};
        uint32_t previousBackingHeight{0};
        uint32_t previousStride{0};
        size_t previousShmSize{0};
        ResizeTransitionPhase resizeTransitionPhase{ResizeTransitionPhase::None};
        std::chrono::steady_clock::time_point resizeDeadline{};
        float resizeCrossfadeElapsedSec{0.0f};
        float resizeCrossfadeProgress{1.0f};
        bool resizeBufferReady{true};
        bool rollbackRequested{false};
        uint64_t resizeGeometryGeneration{0};
        int rollbackX{0};
        int rollbackY{0};
        int rollbackWidth{0};
        int rollbackHeight{0};
        bool rollbackWasMaximized{false};
        bool rollbackWasMinimized{false};

        struct PendingDmaBufRelease {
            uint32_t bufferId{0};
            uint32_t texture{0};
        };
        // Drained only after a compositor presentation. This is the client
        // reuse barrier for the three-slot GBM pool.
        std::vector<PendingDmaBufRelease> pendingDmaBufReleases;

        bool hasRenderableBuffer() const noexcept {
            return pixels != nullptr || dmaBufTexture != 0;
        }
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

    /** Release an entry's mapped SHM or imported DMA-BUF without erasing metadata. */
    static void releaseBuffer(SurfaceEntry& entry) noexcept;
    static void releasePreviousBuffer(SurfaceEntry& entry) noexcept;
    /** Cancel a superseded geometry transaction without releasing the current frame. */
    static void interruptGeometryTransaction(SurfaceEntry& entry,
                                             uint64_t newGeneration) noexcept;
    /**
     * Begin a maximize/restore buffer transaction while retaining the current
     * frame as the rollback and crossfade source.
     */
    static void beginGeometryTransition(SurfaceEntry& entry,
                                        uint64_t newGeneration,
                                        int rollbackX, int rollbackY,
                                        int rollbackWidth, int rollbackHeight,
                                        bool rollbackWasMaximized,
                                        bool rollbackWasMinimized) noexcept;
    static bool acceptsBufferCommit(const SurfaceEntry& entry,
                                    uint64_t configureSerial) noexcept;
    static bool hasOutstandingConfigure(const SurfaceEntry& entry) noexcept;
    static bool hasUnpresentedLiveFrame(const SurfaceEntry& entry) noexcept;
    static void queueLivePresentation(SurfaceEntry& entry,
                                      uint64_t configureSerial) noexcept;
    static void completeLivePresentation(SurfaceEntry& entry) noexcept;
    /** A process may own several independent surface sockets; disconnect is per socket. */
    static bool isOwnedByClientConnection(const SurfaceEntry& entry,
                                          int clientFd) noexcept;

private:
    Entries m_entries;
};

} // namespace lcl::core
