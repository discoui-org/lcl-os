#include <gtest/gtest.h>

#include "lcl-gfxstream/guest_socket_iostream.hpp"

#include <array>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::gfxstream {
namespace {

TEST(GuestSocketIOStreamTest, PreservesTheUpstreamEncoderByteStream) {
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors), 0);
    GuestSocketIOStream guest{
        gpu::GfxstreamPacketStream{client::OwnedFd(descriptors[0])}, 64};
    gpu::GfxstreamPacketStream host{client::OwnedFd(descriptors[1])};

    constexpr std::array<std::uint8_t, 5> encoded{3, 1, 4, 1, 5};
    ASSERT_EQ(guest.writeFully(encoded.data(), encoded.size()), 0);
    std::array<std::uint8_t, encoded.size()> decoded{};
    ASSERT_TRUE(host.readFully(decoded.data(), decoded.size()));
    EXPECT_EQ(decoded, encoded);

    constexpr std::array<std::uint8_t, 3> response{9, 2, 6};
    ASSERT_TRUE(host.writeFully(response.data(), response.size()));
    std::array<std::uint8_t, response.size()> received{};
    EXPECT_EQ(guest.readFully(received.data(), received.size()), received.data());
    EXPECT_EQ(received, response);
}

} // namespace
} // namespace lcl::gfxstream
