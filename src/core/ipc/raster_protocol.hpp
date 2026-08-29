#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lcl::raster_protocol {

inline constexpr uint32_t kMagic = 0x5254434c; // "LCTR"
inline constexpr uint32_t kVersion = 5;
inline constexpr uint32_t kMaxPayload = 1024u * 1024u;

enum class Opcode : uint32_t {
    RegisterSurface = 1,
    RevokeSurface = 2,
    UploadImage = 3,
    SubmitFrame = 4,
    LayerReady = 5,
    ReleaseLayer = 6,
    FrameDiscarded = 7,
    Ready = 8,
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

struct SubmitFrame {
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
    uint32_t displayListSize{0};
    uint32_t flags{0};
};

inline constexpr uint32_t kSubmitReplacesScene = 1u << 0;

enum class LayerTransport : uint32_t {
    Shm = 0,
    DmaBuf = 1,
    AndroidHardwareBuffer = 2,
};

struct LayerReady {
    SurfaceGrant grant{};
    uint64_t layerId{0};
    /** Stable identity of one reusable producer buffer; zero for SHM layers. */
    uint64_t bufferId{0};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t backingWidth{0};
    uint32_t backingHeight{0};
    uint32_t stride{0};
    uint32_t damageX{0};
    uint32_t damageY{0};
    uint32_t damageWidth{0};
    uint32_t damageHeight{0};
    LayerTransport transport{LayerTransport::Shm};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    uint64_t byteSize{0};
};

enum class LayerReleaseReason : uint32_t {
    Presented = 0,
    RejectedTransport = 1,
    RejectedFrame = 2,
    SurfaceRevoked = 3,
};

struct ReleaseLayer {
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

template <typename T>
bool sendPacket(int fd, Opcode opcode, const T& payload, int passedFd = -1) {
    return sendPacket(fd, opcode, &payload, static_cast<uint32_t>(sizeof(T)),
                      passedFd);
}

ReceiveStatus receivePacket(int fd, Header& header,
                            std::vector<uint8_t>& payload, int& receivedFd);

template <typename T>
const T* payloadAs(const Header& header, std::span<const uint8_t> payload,
                   Opcode expected) noexcept {
    if (header.opcode != expected || payload.size() != sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(payload.data());
}

} // namespace lcl::raster_protocol
