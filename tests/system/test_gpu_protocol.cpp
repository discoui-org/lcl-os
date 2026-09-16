#include <gtest/gtest.h>

#include "system/ipc/gpu_protocol.hpp"

#include <array>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lcl::gpu_protocol {

TEST(GpuProtocolTest, RoundTripsAHelloPacket) {
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         sockets.data()), 0);
    Hello sent{};
    sent.nonce = 42;
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::Hello, &sent, sizeof(sent)));

    Header header{};
    std::vector<uint8_t> payload;
    ASSERT_EQ(receivePacket(sockets[1], header, payload), ReceiveStatus::Received);
    const Hello* decoded = payloadAs<Hello>(header, payload, Opcode::Hello);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->requestedVersion, kVersion);
    EXPECT_EQ(decoded->nonce, 42u);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(GpuProtocolTest, RejectsWrongVersion) {
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         sockets.data()), 0);
    Header sent{};
    sent.version = kVersion + 1;
    ASSERT_EQ(send(sockets[0], &sent, sizeof(sent), MSG_NOSIGNAL),
              static_cast<ssize_t>(sizeof(sent)));

    Header header{};
    std::vector<uint8_t> payload;
    EXPECT_EQ(receivePacket(sockets[1], header, payload), ReceiveStatus::Invalid);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(GpuProtocolTest, RejectsTruncatedPayload) {
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         sockets.data()), 0);
    Header sent{};
    sent.opcode = Opcode::Hello;
    sent.payloadSize = sizeof(Hello);
    ASSERT_EQ(send(sockets[0], &sent, sizeof(sent), MSG_NOSIGNAL),
              static_cast<ssize_t>(sizeof(sent)));

    Header header{};
    std::vector<uint8_t> payload;
    EXPECT_EQ(receivePacket(sockets[1], header, payload), ReceiveStatus::Invalid);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(GpuProtocolTest, TransfersFdOwnershipWithOnePacket) {
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         sockets.data()), 0);
    const int source = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(source, 0);
    ClearColor request{};
    request.requestId = 8;
    ASSERT_TRUE(sendPacketWithFd(sockets[0], Opcode::ClearColor,
                                 &request, sizeof(request), source));
    close(source);

    Header header{};
    std::vector<uint8_t> payload;
    int received = -1;
    ASSERT_EQ(receivePacketWithFd(sockets[1], header, payload, received),
              ReceiveStatus::Received);
    ASSERT_NE(payloadAs<ClearColor>(header, payload, Opcode::ClearColor), nullptr);
    ASSERT_GE(received, 0);
    EXPECT_GE(fcntl(received, F_GETFD), 0);
    close(received);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(GpuProtocolTest, RoundTripsOpaqueGfxstreamStreamNegotiation) {
    std::array<int, 2> sockets{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         sockets.data()), 0);
    const OpenGfxstreamStream request{};
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::OpenGfxstreamStream,
                           &request, sizeof(request)));

    Header header{};
    std::vector<uint8_t> payload;
    ASSERT_EQ(receivePacket(sockets[1], header, payload), ReceiveStatus::Received);
    const auto* decoded = payloadAs<OpenGfxstreamStream>(
        header, payload, Opcode::OpenGfxstreamStream);
    ASSERT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->transportVersion, kGfxstreamTransportVersion);
    EXPECT_EQ(decoded->flags, 0u);
    close(sockets[0]);
    close(sockets[1]);
}

} // namespace lcl::gpu_protocol
