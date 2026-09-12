#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lcl::raster_protocol {

inline constexpr uint32_t kMagic = 0x5254434c; // "LCTR"
// v11 adds an atomic retained-presentation manifest.  A LayerReady packet
// only makes immutable storage importable; it never makes that storage
// visible.  The compositor promotes storage exclusively after receiving the
// complete PresentationFrameReady manifest that references it.
inline constexpr uint32_t kVersion = 11;
inline constexpr uint32_t kMaxPayload = 1024u * 1024u;

enum class Opcode : uint32_t {
    RegisterSurface = 1,
    RevokeSurface = 2,
    UploadImage = 3,
    CommitTransaction = 4,
    LayerReady = 5,
    ReleaseLayer = 6,
    FrameDiscarded = 7,
    Ready = 8,
    UploadExternalBuffer = 9,
    SetExternalBufferFence = 10,
    ExternalBufferReleased = 11,
    PresentationFrameReady = 12,
    PresentationAnimation = 13,
    PresentationAnimationResult = 14,
};

enum class ReceiveStatus {
    Received,
    WouldBlock,
    Closed,
    Invalid,
    Error,
};

struct Header {
    uint32_t magic{kMagic};
    uint32_t version{kVersion};
    Opcode opcode{Opcode::Ready};
    uint32_t payloadSize{0};
};

struct SurfaceGrant {
    uint32_t surfaceId{0};
    int32_t ownerPid{0};
    uint32_t flags{0};
    uint64_t tokenHigh{0};
    uint64_t tokenLow{0};
};

inline constexpr uint32_t kGrantInteractiveSystem = 1u << 0;

struct UploadImage {
    SurfaceGrant grant{};
    uint64_t resourceId{0};
    uint64_t contentRevision{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stridePixels{0};
    uint32_t opaque{0};
    uint64_t byteSize{0};
};

enum class ExternalBufferTransport : uint32_t {
    ImmutableShmArgb8888 = 0,
    DmaBufArgb8888 = 1,
};

struct UploadExternalBuffer {
    SurfaceGrant grant{};
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    ExternalBufferTransport transport{ExternalBufferTransport::DmaBufArgb8888};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    uint64_t byteSize{0};
    uint32_t flags{0};
    uint32_t reserved{0};
};

struct SetExternalBufferFence {
    SurfaceGrant grant{};
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
};

enum class ExternalBufferReleaseReason : uint32_t {
    Superseded = 0,
    SurfaceRevoked = 1,
    Rejected = 2,
};

struct ExternalBufferReleased {
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    ExternalBufferReleaseReason reason{ExternalBufferReleaseReason::Superseded};
    uint32_t reserved{0};
};

enum class NodeMutationType : uint32_t {
    CreateNode = 1,
    UpdateContent = 2,
    SetProperties = 3,
    RemoveNode = 4,
};

inline constexpr uint32_t kNodeHasClip = 1u << 0;
inline constexpr uint32_t kNodeHasExternalBuffer = 1u << 1;

/**
 * Backend-neutral retained-node state carried only between lcl-ui and rasterd.
 * Coordinates remain logical; rasterd applies CommitTransaction::bufferScale.
 */
struct RetainedNodeState {
    uint64_t id{0};
    uint64_t parentId{0};
    uint64_t contentRevision{0};
    uint64_t propertyRevision{0};
    uint32_t boundaryReasons{0};
    uint32_t siblingIndex{0};
    uint32_t flags{0};
    uint32_t reserved{0};
    uint64_t externalBufferId{0};
    uint64_t externalBufferRevision{0};
    float layoutX{0.0f};
    float layoutY{0.0f};
    float layoutWidth{0.0f};
    float layoutHeight{0.0f};
    float presentationX{0.0f};
    float presentationY{0.0f};
    float presentationWidth{0.0f};
    float presentationHeight{0.0f};
    float clipX{0.0f};
    float clipY{0.0f};
    float clipWidth{0.0f};
    float clipHeight{0.0f};
    float opacity{1.0f};
    float translationX{0.0f};
    float translationY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotationRadians{0.0f};
    float originX{0.5f};
    float originY{0.5f};
};

struct NodeMutation {
    NodeMutationType type{NodeMutationType::CreateNode};
    uint32_t reserved{0};
    RetainedNodeState node{};
};

struct CommitTransaction {
    SurfaceGrant grant{};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    /** Zero for a complete replacement; otherwise the retained frame to patch. */
    uint64_t baseFrameSerial{0};
    uint64_t geometryGeneration{0};
    float logicalWidth{0.0f};
    float logicalHeight{0.0f};
    float bufferScale{1.0f};
    float damageX{0.0f};
    float damageY{0.0f};
    float damageWidth{0.0f};
    float damageHeight{0.0f};
    /** Zero only for a validated retained-property commit; then no fd exists. */
    uint32_t displayListSize{0};
    uint32_t mutationCount{0};
    uint32_t flags{0};
    uint32_t reserved{0};
    // Zero unless the client enabled frame tracing. All values use the same
    // steady-clock epoch, allowing an accepted frame to be correlated through
    // lcl-ui, rasterd and the compositor without exposing app content.
    uint64_t clientFrameStartNs{0};
    uint64_t clientSubmitNs{0};
};

inline constexpr uint32_t kTransactionReplacesTree = 1u << 0;

enum class LayerTransport : uint32_t {
    Shm = 0,
    DmaBuf = 1,
    AndroidHardwareBuffer = 2,
};

/** The root scene is a normal layer; sublayers use local node coordinates. */
enum class LayerRole : uint32_t {
    Root = 0,
    Presentation = 1,
};

/**
 * The packet descriptor is SHM/DMA-BUF storage for those transports. For an
 * AndroidHardwareBuffer it is the optional acquire fence; the matching AHB
 * handle is queued in FIFO order on rasterd's private native-buffer channel
 * before this readiness packet becomes visible.
 */
struct LayerReady {
    SurfaceGrant grant{};
    uint64_t layerId{0};
    LayerRole role{LayerRole::Root};
    uint32_t reserved{0};
    /** Stable render-node identity; layerId changes only with its content. */
    uint64_t nodeId{0};
    uint64_t contentRevision{0};
    /** Stable identity of one reusable producer buffer; zero for SHM layers. */
    uint64_t bufferId{0};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t backingWidth{0};
    uint32_t backingHeight{0};
    /**
     * Byte stride for byte-addressed SHM/DMA-BUF storage. AndroidHardwareBuffer
     * is imported as an opaque EGL image and may report zero for GPU-only
     * allocations, so zero is valid for that transport.
     */
    uint32_t stride{0};
    uint32_t damageX{0};
    uint32_t damageY{0};
    uint32_t damageWidth{0};
    uint32_t damageHeight{0};
    LayerTransport transport{LayerTransport::Shm};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    uint64_t byteSize{0};
    // Trace-only timestamps copied from CommitTransaction and completed by
    // rasterd immediately before this immutable layer is published.
    uint64_t clientFrameStartNs{0};
    uint64_t clientSubmitNs{0};
    uint64_t rasterStartNs{0};
    uint64_t rasterReadyNs{0};
};

/**
 * One immutable layer placement in a complete presentation snapshot.
 * Geometry is in the toplevel's logical coordinate system.  The compositor
 * applies origin -> scale/rotation -> translation, then opacity and clip.
 */
struct PresentationLayerState {
    uint64_t nodeId{0};
    uint64_t layerId{0};
    uint64_t contentRevision{0};
    uint32_t zOrder{0};
    uint32_t flags{0};
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float opacity{1.0f};
    float translationX{0.0f};
    float translationY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotationRadians{0.0f};
    float originX{0.5f};
    float originY{0.5f};
    float clipX{0.0f};
    float clipY{0.0f};
    float clipWidth{0.0f};
    float clipHeight{0.0f};
};

inline constexpr uint32_t kPresentationLayerHasClip = 1u << 0u;
inline constexpr uint32_t kPresentationLayerOpaque = 1u << 1u;

/** Header followed by exactly layerCount PresentationLayerState records. */
struct PresentationFrameReady {
    SurfaceGrant grant{};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    uint64_t rootNodeId{0};
    uint32_t layerCount{0};
    uint32_t reserved{0};
};

enum class PresentationAnimationCurve : uint32_t {
    Tween = 0,
    Spring = 1,
};

enum class PresentationAnimationOutcome : uint32_t {
    Completed = 0,
    Canceled = 1,
    Rejected = 2,
};

inline constexpr uint32_t kAnimationTranslation = 1u << 0u;
inline constexpr uint32_t kAnimationScale = 1u << 1u;
inline constexpr uint32_t kAnimationRotation = 1u << 2u;
inline constexpr uint32_t kAnimationOpacity = 1u << 3u;

/** One compositor-owned animation declaration for an existing node. */
struct PresentationAnimation {
    SurfaceGrant grant{};
    uint64_t transactionId{0};
    uint64_t nodeId{0};
    uint32_t propertyMask{0};
    PresentationAnimationCurve curve{PresentationAnimationCurve::Tween};
    float durationSec{0.0f};
    float initialVelocityX{0.0f};
    float initialVelocityY{0.0f};
    float initialVelocityScale{0.0f};
    float initialVelocityRotation{0.0f};
    float initialVelocityOpacity{0.0f};
    float startTranslationX{0.0f};
    float startTranslationY{0.0f};
    float startScaleX{1.0f};
    float startScaleY{1.0f};
    float startRotationRadians{0.0f};
    float startOpacity{1.0f};
    float targetTranslationX{0.0f};
    float targetTranslationY{0.0f};
    float targetScaleX{1.0f};
    float targetScaleY{1.0f};
    float targetRotationRadians{0.0f};
    float targetOpacity{1.0f};
    float springMass{1.0f};
    float springStiffness{0.0f};
    float springDamping{0.0f};
};

struct PresentationAnimationResult {
    SurfaceGrant grant{};
    uint64_t transactionId{0};
    uint64_t nodeId{0};
    PresentationAnimationOutcome outcome{
        PresentationAnimationOutcome::Rejected};
};

enum class LayerReleaseReason : uint32_t {
    Presented = 0,
    RejectedTransport = 1,
    RejectedFrame = 2,
    SurfaceRevoked = 3,
};

struct ReleaseLayer {
    /** A Presented release may carry one GPU completion fence descriptor. */
    uint64_t layerId{0};
    LayerReleaseReason reason{LayerReleaseReason::Presented};
};

enum class DiscardReason : uint32_t {
    Superseded = 1,
    InvalidGrant = 2,
    InvalidFrame = 3,
    RasterFailure = 4,
    SurfaceRevoked = 5,
};

struct FrameDiscarded {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    DiscardReason reason{DiscardReason::InvalidFrame};
};

bool sendPacket(int fd, Opcode opcode, const void* payload,
                uint32_t payloadSize, int passedFd = -1);

bool sendCommitTransaction(
    int fd, const CommitTransaction& transaction,
    std::span<const NodeMutation> mutations, int displayListFd);

bool decodeCommitTransaction(
    const Header& header, std::span<const uint8_t> payload,
    CommitTransaction& transaction, std::vector<NodeMutation>& mutations);

bool sendPresentationFrameReady(
    int fd, const PresentationFrameReady& frame,
    std::span<const PresentationLayerState> layers);

bool decodePresentationFrameReady(
    const Header& header, std::span<const uint8_t> payload,
    PresentationFrameReady& frame,
    std::vector<PresentationLayerState>& layers);

/** Validate and send one compositor-owned retained-presentation animation. */
bool sendPresentationAnimation(
    int fd, const PresentationAnimation& animation);

/** Decode exactly one validated animation declaration without an FD. */
bool decodePresentationAnimation(
    const Header& header, std::span<const uint8_t> payload,
    PresentationAnimation& animation);

template <typename T>
bool sendPacket(int fd, Opcode opcode, const T& payload, int passedFd = -1) {
    return sendPacket(fd, opcode, &payload, static_cast<uint32_t>(sizeof(T)),
                      passedFd);
}

// Transport metadata only; never serialized or supplied by the wire payload.
struct SenderCredentials {
    int32_t pid{0};
    uint32_t uid{0};
    uint32_t gid{0};
    bool operator==(const SenderCredentials&) const = default;
};

// A non-null sender requires kernel SCM_CREDENTIALS (SO_PASSCRED must be
// enabled before accepting producer connections). Missing credentials fail
// closed. Private compositor channels do not require this metadata.
ReceiveStatus receivePacket(int fd, Header& header,
                            std::vector<uint8_t>& payload, int& receivedFd,
                            SenderCredentials* sender = nullptr);

template <typename T>
const T* payloadAs(const Header& header, std::span<const uint8_t> payload,
                   Opcode expected) noexcept {
    if (header.opcode != expected || payload.size() != sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(payload.data());
}

} // namespace lcl::raster_protocol
