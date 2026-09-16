#pragma once

#include "lcl-client/owned_fd.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lcl::gpu {

/**
 * A framed Unix SOCK_SEQPACKET connection presented as a reliable byte
 * stream.  The descriptor must already have been granted through the
 * graphics.gpu capability; this type never opens a filesystem path itself.
 */
class GfxstreamPacketStream final {
public:
    static constexpr std::size_t kMaximumPacketBytes = 60 * 1024;

    explicit GfxstreamPacketStream(client::OwnedFd descriptor) noexcept;
    ~GfxstreamPacketStream() = default;
    GfxstreamPacketStream(const GfxstreamPacketStream&) = delete;
    GfxstreamPacketStream& operator=(const GfxstreamPacketStream&) = delete;
    GfxstreamPacketStream(GfxstreamPacketStream&&) noexcept = default;
    GfxstreamPacketStream& operator=(GfxstreamPacketStream&&) noexcept = default;

    bool valid() const noexcept;
    int fd() const noexcept;
    void close() noexcept;

    /** Writes all bytes in ordered, bounded SOCK_SEQPACKET messages. */
    bool writeFully(const void* bytes, std::size_t count);
    /** Blocks until exactly count bytes are available or the peer fails. */
    bool readFully(void* bytes, std::size_t count);
    /** Reads up to count bytes, blocking for at least one byte when count is nonzero. */
    std::size_t readSome(void* bytes, std::size_t count);

private:
    bool refill();

    client::OwnedFd descriptor_;
    std::vector<std::uint8_t> unread_;
    std::size_t unreadOffset_{0};
};

} // namespace lcl::gpu
