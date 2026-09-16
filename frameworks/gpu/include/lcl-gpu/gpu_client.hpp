#pragma once

#include "lcl-client/owned_fd.hpp"
#include "lcl-gpu/gfxstream_packet_stream.hpp"
#include "system/ipc/gpu_protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lcl::gpu {

struct DeviceCapabilities {
    uint32_t vulkanApiVersion{0};
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    uint32_t maxImageDimension2D{0};
    bool supportsAndroidHardwareBuffer{false};
    bool supportsExternalFenceFd{false};
    std::string deviceName;
};

enum class HandshakeStatus {
    Pending,
    Ready,
    Closed,
    ProtocolError,
    IoError,
};

struct ClearedNativeBuffer {
    uint64_t bufferId{0};
    uint64_t contentRevision{1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    /** Owned sync_file acquire fence; invalid means the GPU work is complete. */
    client::OwnedFd acquireFence{};
};

/**
 * Minimal non-blocking control-plane client for lcl-gpud.
 *
 * This deliberately exposes no Vulkan commands or native GPU handles. The
 * future virtual Vulkan ICD will use this connection only after its command
 * marshalling and per-client object model have been defined.
 */
class GpuClient final {
public:
    GpuClient() = default;
    ~GpuClient();
    GpuClient(const GpuClient&) = delete;
    GpuClient& operator=(const GpuClient&) = delete;
    GpuClient(GpuClient&& other) noexcept;
    GpuClient& operator=(GpuClient&& other) noexcept;

    bool connect(const std::string& socketPath = "/Runtime/lcl-gpu.sock");
    void disconnect() noexcept;
    bool connected() const noexcept;
    int fd() const noexcept;

    /** Sends the only currently defined request. Never blocks. */
    bool beginHandshake(uint64_t nonce);

    /** Reads at most one queued reply. Call after fd() becomes readable. */
    HandshakeStatus dispatch(DeviceCapabilities& capabilities);

    /**
     * Transfers the already capability-authorized socket into gfxstream's
     * opaque byte-stream mode.  It succeeds only after a DeviceInfo reply;
     * no Vulkan opcode is added to lcl-gpu-protocol.
     */
    std::optional<GfxstreamPacketStream> openGfxstreamStream();

    /**
     * PoC-only gfxstream-shaped command. It asks native gpud to clear one
     * Android Hardware Buffer; no Vulkan handles are exposed to the rootfs.
     */
    bool clearColor(uint64_t requestId, uint32_t width, uint32_t height,
                    uint32_t format, uint32_t usage, float red, float green,
                    float blue, float alpha, ClearedNativeBuffer& result);

    /** Sends a duplicate of sidebandFd to gpud for exactly one buffer; caller retains it. */
    bool deliverNativeBuffer(uint64_t bufferId, int sidebandFd);

    /** Sends releaseFence to gpud then consumes the caller-owned local FD; it may be invalid. */
    bool releasePresentedBuffer(uint64_t bufferId, client::OwnedFd releaseFence);

private:
    int fd_{-1};
    bool handshakeRequested_{false};
    bool handshakeComplete_{false};
    bool receiveExpected(lcl::gpu_protocol::Opcode expected, uint32_t timeoutMs,
                         lcl::gpu_protocol::Header& header,
                         std::vector<uint8_t>& payload, client::OwnedFd& descriptor);
};

} // namespace lcl::gpu
