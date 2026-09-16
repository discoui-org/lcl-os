#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lcl::gpu_protocol {

// This is the control plane between a rootfs client and the native lcl-gpud
// daemon. The PoC command set is deliberately limited to a GPU clear into an
// Android Hardware Buffer; it is not a general Vulkan command stream.
inline constexpr uint32_t kMagic = 0x5047434c; // "LCGP"
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxPayload = 4096;
inline constexpr uint32_t kAndroidHardwareBufferRgba8888 = 1;
inline constexpr uint32_t kAndroidHardwareBufferGpuSampledColorOutput =
    (1u << 8) | (1u << 9);

enum class Opcode : uint32_t {
    Hello = 1,
    DeviceInfo = 2,
    Error = 3,
    ClearColor = 10,
    ClearColorReady = 11,
    DeliverNativeBuffer = 12,
    DeliveryComplete = 13,
    ReleasePresentedBuffer = 14,
    /** Switches one already-authorized socket into the opaque gfxstream stream mode. */
    OpenGfxstreamStream = 20,
    GfxstreamStreamReady = 21,
    /**
     * Allocates an opaque, gfxstream-owned render target.  This is a
     * presentation-resource control operation, not a Vulkan command: guest
     * Vulkan reaches the target through upstream VK_ANDROID_native_buffer.
     */
    CreateGfxstreamColorBuffer = 30,
    GfxstreamColorBufferReady = 31,
    DestroyGfxstreamColorBuffer = 32,
};

struct Header {
    uint32_t magic{kMagic};
    uint32_t version{kVersion};
    Opcode opcode{Opcode::Error};
    uint32_t payloadSize{0};
};

struct Hello {
    uint32_t requestedVersion{kVersion};
    uint32_t flags{0};
    uint64_t nonce{0};
};

inline constexpr uint32_t kDeviceSupportsAndroidHardwareBuffer = 1u << 0;
inline constexpr uint32_t kDeviceSupportsExternalFenceFd = 1u << 1;

struct DeviceInfo {
    uint32_t vulkanApiVersion{0};
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    uint32_t maxImageDimension2D{0};
    uint32_t flags{0};
    char deviceName[256]{};
};

struct Error {
    uint32_t code{0};
    char message[256]{};
};

/** One gfxstream-shaped guest command for the brokered-presentation PoC. */
struct ClearColor {
    uint64_t requestId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t format{0};
    uint32_t usage{0};
    float red{0.0f};
    float green{0.0f};
    float blue{0.0f};
    float alpha{1.0f};
};

/** The attached FD is an owned sync_file acquire fence, or absent if signaled. */
struct ClearColorReady {
    uint64_t requestId{0};
    uint64_t bufferId{0};
    uint64_t contentRevision{1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
};

/** SCM_RIGHTS carries SurfaceClient's already-registered raster sideband FD. */
struct DeliverNativeBuffer { uint64_t bufferId{0}; };
struct DeliveryComplete { uint64_t bufferId{0}; uint32_t status{0}; uint32_t reserved{0}; };
/** SCM_RIGHTS optionally carries the compositor release sync_file. */
struct ReleasePresentedBuffer { uint64_t bufferId{0}; };

/**
 * Control-plane negotiation only.  After GfxstreamStreamReady, both peers
 * exchange opaque byte packets directly; this protocol never describes
 * Vulkan commands or object handles.
 */
inline constexpr uint32_t kGfxstreamTransportVersion = 1;
struct OpenGfxstreamStream {
    uint32_t transportVersion{kGfxstreamTransportVersion};
    uint32_t flags{0};
};
struct GfxstreamStreamReady {
    uint32_t transportVersion{kGfxstreamTransportVersion};
    uint32_t reserved{0};
};

/** Restricted to a single-plane RGBA8888 target in the initial bridge. */
struct CreateGfxstreamColorBuffer {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t format{0};
    uint32_t flags{0};
};

/** colorBufferHandle is an opaque upstream gfxstream identifier, never a pointer. */
struct GfxstreamColorBufferReady {
    uint64_t targetId{0};
    uint32_t colorBufferHandle{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t format{0};
    uint32_t stride{0};
};

struct DestroyGfxstreamColorBuffer { uint64_t targetId{0}; };

enum class ReceiveStatus { Received, WouldBlock, Closed, Invalid, Error };

bool sendPacket(int socketFd, Opcode opcode, const void* payload,
                uint32_t payloadSize);
/**
 * Sends a duplicate of descriptor with this packet. The caller retains and
 * must close its original descriptor regardless of success or failure.
 */
bool sendPacketWithFd(int socketFd, Opcode opcode, const void* payload,
                      uint32_t payloadSize, int descriptor);
/** On Received, receivedDescriptor is owned by the caller and must be closed. */
ReceiveStatus receivePacket(int socketFd, Header& header,
                            std::vector<uint8_t>& payload);
ReceiveStatus receivePacketWithFd(int socketFd, Header& header,
                                  std::vector<uint8_t>& payload,
                                  int& receivedDescriptor);

template <typename T>
const T* payloadAs(const Header& header, std::span<const uint8_t> payload,
                   Opcode expectedOpcode) {
    if (header.opcode != expectedOpcode || payload.size() != sizeof(T)) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(payload.data());
}

} // namespace lcl::gpu_protocol
