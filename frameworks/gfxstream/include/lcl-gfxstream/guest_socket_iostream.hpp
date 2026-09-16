#pragma once

#include "lcl-gpu/gfxstream_packet_stream.hpp"
#include "lcl-gpu/gpu_client.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gfxstream/guest/IOStream.h>

namespace lcl::gfxstream {

/**
 * LCL-owned implementation of gfxstream's guest IOStream contract.
 *
 * Its packet stream must originate from lcl-gpu-client after an authorized
 * graphics.gpu handshake.  This adapter is deliberately ignorant of Vulkan
 * opcodes: it carries the upstream encoder's existing byte stream unchanged.
 */
class GuestSocketIOStream final : public ::gfxstream::guest::IOStream {
public:
    /** Opens only the granted lcl-gpu-client endpoint and negotiates its stream mode. */
    static std::unique_ptr<GuestSocketIOStream> openAuthorized(
        const std::string& socketPath, std::uint64_t nonce,
        gpu::DeviceCapabilities* capabilities = nullptr);

    explicit GuestSocketIOStream(gpu::GfxstreamPacketStream stream,
                                 std::size_t bufferSize = 4 * 1024 * 1024);
    ~GuestSocketIOStream() override;
    GuestSocketIOStream(const GuestSocketIOStream&) = delete;
    GuestSocketIOStream& operator=(const GuestSocketIOStream&) = delete;

    bool valid() const noexcept;

    void* allocBuffer(std::size_t minimumSize) override;
    int commitBuffer(std::size_t size) override;
    const unsigned char* readFully(void* buffer, std::size_t size) override;
    const unsigned char* commitBufferAndReadFully(std::size_t writeSize, void* buffer,
                                                   std::size_t readSize) override;
    const unsigned char* read(void* buffer, std::size_t* inOutSize) override;
    int writeFully(const void* buffer, std::size_t size) override;

private:
    gpu::GfxstreamPacketStream stream_;
    std::vector<std::uint8_t> buffer_;
};

} // namespace lcl::gfxstream
