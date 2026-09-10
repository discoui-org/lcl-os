#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "system/ipc/lcl_protocol.hpp"
#include "system/ipc/raster_protocol.hpp"

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
            Interactive,
        };

        uint32_t windowId{0};
        uint64_t parentSurfaceKey{0};
        uint32_t attachedWindowId{0};
        protocol::LCLAttachedSurfaceRole attachedRole{
            protocol::LCLAttachedSurfaceRole::Adornment};
        float attachedX{0.0f};
        float attachedY{0.0f};
        float attachedWidth{0.0f};
        float attachedHeight{0.0f};
        bool attachedFollowParentWidth{false};
        bool attachedFollowParentHeight{false};
        bool attachedAcceptsInput{false};
        uint64_t attachmentOrder{0};
        protocol::LCLPopupRole popupRole{protocol::LCLPopupRole::Transient};
        float popupX{0.0f};
        float popupY{0.0f};
        uint64_t popupOrder{0};
        int clientFd{-1};
        raster_protocol::SurfaceGrant producerGrant{};
        struct RasterLayerRelease {
            uint64_t layerId{0};
            uint32_t texture{0};
        };
        uint64_t rasterLayerId{0};
        uint32_t rasterLayerTexture{0};
        uint64_t frameSerial{0};
        uint64_t layerGeometryGeneration{0};
        // Trace-only monotonic timestamps carried with the retained layer.
        uint64_t clientFrameStartNs{0};
        uint64_t clientSubmitNs{0};
        uint64_t rasterStartNs{0};
        uint64_t rasterReadyNs{0};
        std::vector<RasterLayerRelease> pendingRasterLayerReleases;
        int shmFd{-1};
        void* pixels{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t backingWidth{0};
        uint32_t backingHeight{0};
        uint32_t stride{0};
        float bufferScale{1.0f};
        size_t shmSize{0};
        // Monotonic compositor-local identity for the pixels published by the
        // latest SHM commit. Android uses it to retain one GL texture per
        // surface without re-uploading unchanged buffers during scene redraws.
        uint64_t shmContentSerial{0};
        uint32_t shmDamageX{0};
        uint32_t shmDamageY{0};
        uint32_t shmDamageWidth{0};
        uint32_t shmDamageHeight{0};
        // A surface is registered before it is mapped.  Keep its window policy
        // here until the first complete client buffer is ready to present.
        std::string title;
        float initialX{0.0f};
        float initialY{0.0f};
        float initialWidth{0.0f};
        float initialHeight{0.0f};
        float resizeBaseWidth{0.0f};
        float resizeBaseHeight{0.0f};
        float resizeWidthIncrement{0.0f};
        float resizeHeightIncrement{0.0f};
        protocol::LCLSystemSurfaceKind systemSurfaceKind{protocol::LCLSystemSurfaceKind::None};
        protocol::LCLDecorationMode decorationMode{protocol::LCLDecorationMode::SSD};
        bool edgeToEdge{false};
        protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};
        bool unfocusable{false};
        bool insetBorderEnabled{true};
        float cornerRadius{-1.0f};
        float cornerRoundness{2.0f};
        bool suppressInitialTransition{false};
        std::string appId;
        uint64_t appInstanceId{0};
        std::vector<SurfaceEffectRegion> effectRegions;
        // Monotonic compositor-local identity for the current effect graph.
        // Retained backdrop results may be reused only while this is unchanged.
        uint64_t effectRevision{0};

        float configuredX{0.0f};
        float configuredY{0.0f};
        float configuredWidth{0.0f};
        float configuredHeight{0.0f};
        uint8_t configuredFocused{0};
        uint64_t nextConfigureSerial{1};
        uint64_t pendingConfigureSerial{0};
        uint64_t acceptedConfigureSerial{0};
        uint64_t configuredGeometryGeneration{0};
        // Non-zero after rasterd's immutable layer has been accepted and before
        // the compositor frame containing it has been presented.
        uint64_t presentationSerial{0};
        // Metadata-only WindowGroup barrier. Buffer ownership remains in the
        // normal current slot; scanout waits until every participant commits.
        uint64_t atomicConfigureGeneration{0};
        uint32_t atomicConfigureParticipantCount{0};
        bool atomicConfigureIssued{false};
        bool forceConfigure{false};
        std::chrono::steady_clock::time_point lastConfigureSent{};

        TransitionPhase transitionPhase{TransitionPhase::None};
        float transitionElapsedSec{0.0f};
        float transitionDurationSec{0.0f};
        float transitionOpacity{1.0f};
        float transitionScale{1.0f};
        bool hasLaunchOrigin{false};
        float launchOriginX{0.0f};
        float launchOriginY{0.0f};
        float launchOriginWidth{0.0f};
        float launchOriginHeight{0.0f};
        float launchOriginCornerRadius{0.0f};
        uint32_t launchIconWidth{0};
        uint32_t launchIconHeight{0};
        std::vector<uint32_t> launchIconPixels;
        bool launchMorphActive{false};
        float launchMorphX{0.0f};
        float launchMorphY{0.0f};
        float launchMorphWidth{0.0f};
        float launchMorphHeight{0.0f};
        float launchMorphCornerRadius{0.0f};
        float launchMorphCornerRoundness{2.0f};
        uint64_t launchToken{0};
        int launchOwnerFd{-1};
        bool launchIconRevealPending{false};
        // Holds the final compositor proxy until HomeScreen commits the frame
        // containing the stationary icon and acknowledges that commit.
        bool launchIconHandoffActive{false};
        std::chrono::steady_clock::time_point launchIconHandoffDeadline{};
        bool isLaunchPlaceholder{false};
        bool launchPlaceholderActive{false};
        // Bounds the white launch proxy when a process registers but never
        // publishes its first layer. A late first layer may still map normally.
        std::chrono::steady_clock::time_point launchPlaceholderDeadline{};
        bool launchContentFadeActive{false};
        float launchContentOpacity{1.0f};
        float launchContentFadeElapsedSec{0.0f};
        bool launchGestureActive{false};
        float launchGestureStartX{0.0f};
        float launchGestureStartY{0.0f};
        float launchGestureX{0.0f};
        float launchGestureY{0.0f};
        float launchGestureVelocityX{0.0f};
        float launchGestureVelocityY{0.0f};
        bool launchGestureFlingPending{false};
        // Compositor-owned 0=launcher, 1=fullscreen background presentation.
        float launchHomeTransitionProgress{0.0f};
        bool hasCommittedBuffer{false};
        bool ignoreBufferCommits{false};
        bool pendingDestroy{false};
        bool pendingMinimize{false};

        bool hasRenderableBuffer() const noexcept {
            return pixels != nullptr || rasterLayerTexture != 0;
        }
        bool isPopup() const noexcept { return parentSurfaceKey != 0; }
        bool isAttached() const noexcept { return attachedWindowId != 0; }
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

    /** Freeze the last renderable client frame and start its close transition. */
    static bool beginClosingTransition(SurfaceEntry& entry) noexcept;

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
    std::vector<Key> popupChildren(Key parentSurfaceKey) const;
    std::vector<Key> attachedChildren(uint32_t windowId) const;
    uint64_t allocatePopupOrder() noexcept { return m_nextPopupOrder++; }
    uint64_t allocateAttachmentOrder() noexcept { return m_nextAttachmentOrder++; }

    /** Keyboard routing target within the currently focused WindowGroup. */
    Key keyboardFocusSurface() const noexcept { return m_keyboardFocusSurface; }
    bool focusKeyboardSurface(Key key) noexcept;
    /** Return focus from a closing surface to its live parent, when present. */
    void releaseKeyboardFocus(Key key) noexcept;

    iterator erase(iterator position);
    size_t erase(Key key);
    void clear() noexcept;

    /** Release an entry's retained raster layer without erasing metadata. */
    static void releaseBuffer(SurfaceEntry& entry) noexcept;
    /** Cancel a superseded geometry transaction without releasing the current frame. */
    static void interruptGeometryTransaction(SurfaceEntry& entry,
                                             uint64_t newGeneration) noexcept;
    static bool acceptsBufferCommit(const SurfaceEntry& entry,
                                    uint64_t configureSerial) noexcept;
    /** Require a layer to realize the exact compositor-authored configure. */
    static bool matchesConfiguredBufferExtent(const SurfaceEntry& entry,
                                              uint32_t physicalWidth,
                                              uint32_t physicalHeight) noexcept;
    static bool hasOutstandingConfigure(const SurfaceEntry& entry) noexcept;
    static bool hasUnpresentedFrame(const SurfaceEntry& entry) noexcept;
    static void queuePresentation(SurfaceEntry& entry,
                                  uint64_t configureSerial) noexcept;
    static void completePresentation(SurfaceEntry& entry) noexcept;
    /** A process may own several independent surface sockets; disconnect is per socket. */
    static bool isOwnedByClientConnection(const SurfaceEntry& entry,
                                          int clientFd) noexcept;

private:
    Entries m_entries;
    uint64_t m_nextPopupOrder{1};
    uint64_t m_nextAttachmentOrder{1};
    Key m_keyboardFocusSurface{0};
};

} // namespace lcl::core
