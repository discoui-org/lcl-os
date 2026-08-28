#include <gtest/gtest.h>

#include "core/ipc/raster_protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

namespace lcl::raster_protocol {

TEST(RasterProtocolTest, LayerReadyCarriesGenerationAndOnePrivateDescriptor) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);

    LayerReady sent{};
    sent.grant = {7, 42, kGrantInteractiveSystem, 11, 13};
    sent.layerId = 99;
    sent.configureSerial = 17;
    sent.frameSerial = 19;
    sent.geometryGeneration = 23;
    sent.width = 640;
    sent.height = 480;
    sent.backingWidth = 672;
    sent.backingHeight = 512;
    sent.stride = 2560;
    sent.damageWidth = 640;
    sent.damageHeight = 480;
    sent.transport = LayerTransport::DmaBuf;
    sent.format = 0x34325241u;
    sent.modifier = 7;
    sent.byteSize = 640u * 480u * sizeof(uint32_t);
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::LayerReady, sent, descriptor));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    const auto* ready = payloadAs<LayerReady>(header, payload, Opcode::LayerReady);
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->grant.tokenHigh, 11u);
    EXPECT_EQ(ready->grant.tokenLow, 13u);
    EXPECT_EQ(ready->configureSerial, 17u);
    EXPECT_EQ(ready->frameSerial, 19u);
    EXPECT_EQ(ready->geometryGeneration, 23u);
    EXPECT_EQ(ready->backingWidth, 672u);
    EXPECT_EQ(ready->backingHeight, 512u);
    EXPECT_EQ(ready->transport, LayerTransport::DmaBuf);
    EXPECT_EQ(ready->format, 0x34325241u);
    EXPECT_EQ(ready->modifier, 7u);
    EXPECT_GE(receivedFd, 0);

    close(receivedFd);
    close(descriptor);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, RejectsWrongPrivateProtocolVersion) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    Header invalid{};
    invalid.version = kVersion + 1;
    ASSERT_EQ(send(sockets[0], &invalid, sizeof(invalid), MSG_NOSIGNAL),
              static_cast<ssize_t>(sizeof(invalid)));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    EXPECT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Invalid);
    EXPECT_EQ(receivedFd, -1);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(RasterProtocolTest, ReleaseLayerCarriesTransportRejectionReason) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    ReleaseLayer sent{};
    sent.layerId = 73;
    sent.reason = LayerReleaseReason::RejectedTransport;
    ASSERT_TRUE(sendPacket(sockets[0], Opcode::ReleaseLayer, sent));

    Header header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(receivePacket(sockets[1], header, payload, receivedFd),
              ReceiveStatus::Received);
    const auto* received = payloadAs<ReleaseLayer>(
        header, payload, Opcode::ReleaseLayer);
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(received->layerId, 73u);
    EXPECT_EQ(received->reason, LayerReleaseReason::RejectedTransport);
    EXPECT_EQ(receivedFd, -1);

    close(sockets[0]);
    close(sockets[1]);
}

} // namespace lcl::raster_protocol
