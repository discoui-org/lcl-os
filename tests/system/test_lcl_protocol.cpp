#include <gtest/gtest.h>
#include "system/ipc/lcl_protocol.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <array>
#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <limits>

using namespace lcl::protocol;

TEST(LCLProtocolTest, HeaderMagicAndDefaults) {
    LCLHeader header;
    EXPECT_EQ(header.magic, LCL_PROTOCOL_MAGIC);
    EXPECT_EQ(header.version, LCL_PROTOCOL_VERSION);
    EXPECT_EQ(header.opcode, LCLOpcode::AckResponse);
    EXPECT_EQ(header.flags, 0u);
    EXPECT_EQ(header.requestId, 0u);
    EXPECT_EQ(header.payloadSize, 0u);
}
TEST(LCLProtocolTest, SendAndReceiveMsgOverSocketPair) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLHeader headerSend;
    headerSend.magic = LCL_PROTOCOL_MAGIC;
    headerSend.version = LCL_PROTOCOL_VERSION;
    headerSend.opcode = LCLOpcode::SurfaceCreate;
    headerSend.requestId = 77;
    
    LCLMsgSurfaceCreate msg{};
    msg.surfaceId = 42;
    msg.x = 100;
    msg.y = 200;
    msg.width = 800;
    msg.height = 600;
    msg.hasLaunchOrigin = 1;
    msg.launchOriginX = 24.0f;
    msg.launchOriginY = 32.0f;
    msg.launchOriginWidth = 60.0f;
    msg.launchOriginHeight = 60.0f;
    msg.launchOriginCornerRadius = 14.0f;
    msg.launchToken = 73;
    msg.appInstanceId = 91;
    msg.resizeBaseWidth = 16.0f;
    msg.resizeBaseHeight = 16.0f;
    msg.resizeWidthIncrement = 8.0f;
    msg.resizeHeightIncrement = 16.0f;
    std::strncpy(msg.title, "Test Window Title", sizeof(msg.title) - 1);
    std::strncpy(msg.appId, "org.lcl.test", sizeof(msg.appId) - 1);

    headerSend.payloadSize = sizeof(msg);

    bool sendOk = sendMsgWithFd(sv[0], headerSend, &msg, -1);
    EXPECT_TRUE(sendOk);

    LCLHeader headerRecv;
    std::vector<uint8_t> payloadRecv;
    int receivedFd = -1;

    bool recvOk = recvMsgWithFd(sv[1], headerRecv, payloadRecv, receivedFd);
    EXPECT_TRUE(recvOk);
    EXPECT_EQ(receivedFd, -1);

    EXPECT_EQ(headerRecv.magic, LCL_PROTOCOL_MAGIC);
    EXPECT_EQ(headerRecv.version, LCL_PROTOCOL_VERSION);
    EXPECT_EQ(headerRecv.opcode, LCLOpcode::SurfaceCreate);
    EXPECT_EQ(headerRecv.requestId, 77u);
    EXPECT_EQ(headerRecv.payloadSize, sizeof(LCLMsgSurfaceCreate));
    ASSERT_EQ(payloadRecv.size(), sizeof(LCLMsgSurfaceCreate));

    const auto* msgRecv = reinterpret_cast<const LCLMsgSurfaceCreate*>(payloadRecv.data());
    EXPECT_EQ(msgRecv->surfaceId, 42u);
    EXPECT_FLOAT_EQ(msgRecv->x, 100.0f);
    EXPECT_FLOAT_EQ(msgRecv->y, 200.0f);
    EXPECT_FLOAT_EQ(msgRecv->width, 800.0f);
    EXPECT_FLOAT_EQ(msgRecv->height, 600.0f);
    EXPECT_EQ(msgRecv->hasLaunchOrigin, 1);
    EXPECT_FLOAT_EQ(msgRecv->launchOriginX, 24.0f);
    EXPECT_FLOAT_EQ(msgRecv->launchOriginY, 32.0f);
    EXPECT_FLOAT_EQ(msgRecv->launchOriginWidth, 60.0f);
    EXPECT_FLOAT_EQ(msgRecv->launchOriginHeight, 60.0f);
    EXPECT_FLOAT_EQ(msgRecv->launchOriginCornerRadius, 14.0f);
    EXPECT_EQ(msgRecv->launchToken, 73u);
    EXPECT_EQ(msgRecv->appInstanceId, 91u);
    EXPECT_FLOAT_EQ(msgRecv->resizeBaseWidth, 16.0f);
    EXPECT_FLOAT_EQ(msgRecv->resizeBaseHeight, 16.0f);
    EXPECT_FLOAT_EQ(msgRecv->resizeWidthIncrement, 8.0f);
    EXPECT_FLOAT_EQ(msgRecv->resizeHeightIncrement, 16.0f);
    EXPECT_STREQ(msgRecv->title, "Test Window Title");
    EXPECT_STREQ(msgRecv->appId, "org.lcl.test");

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, FrameDiscardedRoundTripsConfigureSerial) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    LCLMsgFrameDiscarded discarded{};
    discarded.surfaceId = 4;
    discarded.configureSerial = 91;
    discarded.frameSerial = 92;
    discarded.geometryGeneration = 7;
    discarded.reason = LCLFrameDiscardReason::Superseded;
    LCLHeader header{};
    header.opcode = LCLOpcode::FrameDiscarded;
    header.payloadSize = sizeof(discarded);
    ASSERT_TRUE(sendMsgWithFd(sockets[0], header, &discarded));

    LCLHeader received{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sockets[1], received, payload, receivedFd));
    ASSERT_EQ(received.opcode, LCLOpcode::FrameDiscarded);
    ASSERT_EQ(payload.size(), sizeof(LCLMsgFrameDiscarded));
    const auto* decoded = reinterpret_cast<const LCLMsgFrameDiscarded*>(payload.data());
    EXPECT_EQ(decoded->surfaceId, 4u);
    EXPECT_EQ(decoded->configureSerial, 91u);
    EXPECT_EQ(decoded->frameSerial, 92u);
    EXPECT_EQ(decoded->geometryGeneration, 7u);
    EXPECT_EQ(decoded->reason, LCLFrameDiscardReason::Superseded);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LCLProtocolTest, PopupSurfaceCreateRoundTripsParentRoleAndGeometry) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    LCLMsgPopupSurfaceCreate popup{};
    popup.surfaceId = 9;
    popup.parentSurfaceId = 3;
    popup.role = LCLPopupRole::Transient;
    popup.x = 280;
    popup.y = -12;
    popup.width = 220;
    popup.height = 96;
    LCLHeader header{};
    header.opcode = LCLOpcode::PopupSurfaceCreate;
    header.payloadSize = sizeof(popup);
    ASSERT_TRUE(sendMsgWithFd(sockets[0], header, &popup));

    LCLHeader received{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sockets[1], received, payload, receivedFd));
    ASSERT_EQ(received.opcode, LCLOpcode::PopupSurfaceCreate);
    ASSERT_EQ(payload.size(), sizeof(LCLMsgPopupSurfaceCreate));
    const auto* decoded = reinterpret_cast<const LCLMsgPopupSurfaceCreate*>(payload.data());
    EXPECT_EQ(decoded->surfaceId, 9u);
    EXPECT_EQ(decoded->parentSurfaceId, 3u);
    EXPECT_EQ(decoded->role, LCLPopupRole::Transient);
    EXPECT_FLOAT_EQ(decoded->x, 280.0f);
    EXPECT_FLOAT_EQ(decoded->y, -12.0f);
    EXPECT_FLOAT_EQ(decoded->width, 220.0f);
    EXPECT_FLOAT_EQ(decoded->height, 96.0f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LCLProtocolTest, PopupSurfaceRejectsSelfParentAndUnknownRole) {
    LCLMsgPopupSurfaceCreate popup{};
    popup.surfaceId = 4;
    popup.parentSurfaceId = 4;
    popup.width = 100;
    popup.height = 50;
    LCLHeader header{};
    header.opcode = LCLOpcode::PopupSurfaceCreate;
    header.payloadSize = sizeof(popup);
    std::vector<uint8_t> packet;
    EXPECT_FALSE(encodePacket(header, &popup, packet));

    popup.parentSurfaceId = 1;
    popup.role = static_cast<LCLPopupRole>(99);
    EXPECT_FALSE(encodePacket(header, &popup, packet));
}

TEST(LCLProtocolTest, AttachedSurfaceRoundTripsGenericParentRelationship) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    LCLMsgAttachedSurfaceCreate attached{};
    attached.surfaceId = 27;
    attached.targetWindowId = 9;
    attached.role = LCLAttachedSurfaceRole::Frame;
    attached.x = 0.0f;
    attached.y = 0.0f;
    attached.width = 720.0f;
    attached.height = 32.0f;
    attached.followParentWidth = 1;
    attached.acceptsInput = 1;
    LCLHeader header{};
    header.opcode = LCLOpcode::AttachedSurfaceCreate;
    header.payloadSize = sizeof(attached);
    ASSERT_TRUE(sendMsgWithFd(sockets[0], header, &attached));

    LCLHeader received{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sockets[1], received, payload, receivedFd));
    ASSERT_EQ(received.opcode, LCLOpcode::AttachedSurfaceCreate);
    ASSERT_EQ(payload.size(), sizeof(LCLMsgAttachedSurfaceCreate));
    const auto* decoded = reinterpret_cast<const
        LCLMsgAttachedSurfaceCreate*>(payload.data());
    EXPECT_EQ(decoded->surfaceId, 27u);
    EXPECT_EQ(decoded->targetWindowId, 9u);
    EXPECT_EQ(decoded->role, LCLAttachedSurfaceRole::Frame);
    EXPECT_FLOAT_EQ(decoded->width, 720.0f);
    EXPECT_FLOAT_EQ(decoded->height, 32.0f);
    EXPECT_EQ(decoded->followParentWidth, 1u);
    EXPECT_EQ(decoded->followParentHeight, 0u);
    EXPECT_EQ(decoded->acceptsInput, 1u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LCLProtocolTest, ConfigureAndAttachRoundTripTheSameSerial) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    LCLMsgConfigureBounds configure{};
    configure.surfaceId = 4;
    configure.configureSerial = 0x0102030405060708ull;
    configure.width = 640;
    configure.height = 480;
    configure.backingWidth = 1920;
    configure.backingHeight = 1080;
    configure.bufferScale = 1.0f;
    configure.resizeReason = LCLConfigureResizeReason::WindowStateTransition;
    LCLHeader header{};
    header.opcode = LCLOpcode::ConfigureBounds;
    header.payloadSize = sizeof(configure);
    ASSERT_TRUE(sendMsgWithFd(sockets[0], header, &configure));

    LCLHeader received{};
    std::vector<uint8_t> payload;
    int fd = -1;
    ASSERT_TRUE(recvMsgWithFd(sockets[1], received, payload, fd));
    ASSERT_EQ(payload.size(), sizeof(LCLMsgConfigureBounds));
    const auto* decoded = reinterpret_cast<const LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(decoded->configureSerial, configure.configureSerial);
    EXPECT_EQ(decoded->backingWidth, 1920u);
    EXPECT_EQ(decoded->backingHeight, 1080u);
    EXPECT_EQ(decoded->resizeReason, LCLConfigureResizeReason::WindowStateTransition);

    close(sockets[0]);
    close(sockets[1]);
}

namespace {

void appendLe32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> surfaceCreatePacket() {
    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceCreate;
    header.requestId = 91;
    header.payloadSize = sizeof(LCLMsgSurfaceCreate);
    LCLMsgSurfaceCreate create{};
    create.surfaceId = 4;
    create.width = 640;
    create.height = 480;
    std::strncpy(create.title, "Codec Test", sizeof(create.title) - 1);
    std::strncpy(create.appId, "org.lcl.codec-test", sizeof(create.appId) - 1);
    std::vector<uint8_t> packet;
    if (!encodePacket(header, &create, packet))
        return {};
    return packet;
}

} // namespace

TEST(LCLProtocolTest, WireHeaderIsExplicitLittleEndian) {
    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceDestroy;
    header.flags = 0;
    header.requestId = 0x11223344u;
    header.payloadSize = sizeof(LCLMsgSurfaceDestroy);
    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 0xA1B2C3D4u;

    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, &destroy, packet));
    ASSERT_EQ(packet.size(), LCL_PROTOCOL_WIRE_HEADER_SIZE + sizeof(destroy));
    EXPECT_EQ(packet[16], 0x44);
    EXPECT_EQ(packet[17], 0x33);
    EXPECT_EQ(packet[18], 0x22);
    EXPECT_EQ(packet[19], 0x11);
    EXPECT_EQ(packet[24], 0xD4);
    EXPECT_EQ(packet[25], 0xC3);
    EXPECT_EQ(packet[26], 0xB2);
    EXPECT_EQ(packet[27], 0xA1);
}

TEST(LCLProtocolTest, RejectsProtocolV2Packet) {
    std::vector<uint8_t> packet;
    appendLe32(packet, LCL_PROTOCOL_MAGIC);
    appendLe32(packet, 2);
    appendLe32(packet, static_cast<uint32_t>(LCLOpcode::SurfaceDestroy));
    appendLe32(packet, 0);
    appendLe32(packet, 1);
    appendLe32(packet, sizeof(LCLMsgSurfaceDestroy));
    appendLe32(packet, 1);

    LCLHeader header{};
    std::vector<uint8_t> payload;
    EXPECT_FALSE(decodePacket(packet.data(), packet.size(), header, payload));
}

TEST(LCLProtocolTest, RejectsTruncatedAndNonFiniteLogicalSurfaceGeometry) {
    auto packet = surfaceCreatePacket();
    ASSERT_FALSE(packet.empty());

    // SurfaceCreate must contain the complete launch context.
    packet.resize(packet.size() - sizeof(uint8_t));
    const uint32_t shortened = sizeof(LCLMsgSurfaceCreate) - sizeof(uint8_t);
    packet[20] = static_cast<uint8_t>(shortened);
    packet[21] = static_cast<uint8_t>(shortened >> 8);
    packet[22] = static_cast<uint8_t>(shortened >> 16);
    packet[23] = static_cast<uint8_t>(shortened >> 24);
    LCLHeader header{};
    std::vector<uint8_t> payload;
    EXPECT_FALSE(decodePacket(packet.data(), packet.size(), header, payload));

    packet = surfaceCreatePacket();
    const uint32_t nanBits =
        std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());
    // First logical float follows surfaceId.
    const size_t xOffset = sizeof(LCLHeader) + sizeof(uint32_t);
    packet[xOffset] = static_cast<uint8_t>(nanBits);
    packet[xOffset + 1] = static_cast<uint8_t>(nanBits >> 8);
    packet[xOffset + 2] = static_cast<uint8_t>(nanBits >> 16);
    packet[xOffset + 3] = static_cast<uint8_t>(nanBits >> 24);
    EXPECT_FALSE(decodePacket(packet.data(), packet.size(), header, payload));

}

TEST(LCLProtocolTest, QueuedPacketsPreserveRequestOrder) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    int sendBufferSize = 4096;
    ASSERT_EQ(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sendBufferSize,
                         sizeof(sendBufferSize)), 0);

    std::array<uint8_t, 512> filler{};
    while (send(sv[0], filler.data(), filler.size(), MSG_DONTWAIT) >= 0) {
    }
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 8;
    LCLHeader first{};
    first.opcode = LCLOpcode::SurfaceDestroy;
    first.requestId = 101;
    first.payloadSize = sizeof(destroy);
    LCLHeader second = first;
    second.requestId = 102;

    ASSERT_TRUE(sendMsgWithFd(sv[0], first, &destroy));
    ASSERT_TRUE(sendMsgWithFd(sv[0], second, &destroy));

    while (recv(sv[1], filler.data(), filler.size(), MSG_DONTWAIT) >= 0) {
    }
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    ASSERT_TRUE(flushPendingWrites(sv[0]));

    for (uint32_t expected : {101u, 102u}) {
        LCLHeader received{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        ASSERT_EQ(recvPacketWithFd(sv[1], received, payload, receivedFd),
                  ReceiveStatus::Received);
        EXPECT_EQ(received.requestId, expected);
    }

    discardPendingWrites(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SeqpacketRejectsTruncatedPayload) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);
    auto packet = surfaceCreatePacket();
    ASSERT_GT(packet.size(), LCL_PROTOCOL_WIRE_HEADER_SIZE);
    iovec iov{packet.data(), packet.size() - 1};
    msghdr rawMessage{};
    rawMessage.msg_iov = &iov;
    rawMessage.msg_iovlen = 1;
    ASSERT_EQ(sendmsg(sv[0], &rawMessage, 0),
              static_cast<ssize_t>(packet.size() - 1));

    LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    EXPECT_EQ(recvPacketWithFd(sv[1], header, payload, receivedFd),
              ReceiveStatus::Invalid);
    EXPECT_EQ(receivedFd, -1);
    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, AppProtocolRejectsAllFileDescriptors) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);
    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 3;
    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceDestroy;
    header.payloadSize = sizeof(destroy);
    EXPECT_FALSE(sendMsgWithFd(sockets[0], header, &destroy, descriptor));
    close(descriptor);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(LCLProtocolTest, ContentExtentMustFitInsideConfiguredBacking) {
    LCLMsgConfigureBounds configure{};
    configure.surfaceId = 1;
    configure.configureSerial = 2;
    configure.width = 801;
    configure.height = 480;
    configure.backingWidth = 800;
    configure.backingHeight = 600;
    configure.bufferScale = 1.0f;
    LCLHeader header{};
    header.opcode = LCLOpcode::ConfigureBounds;
    header.payloadSize = sizeof(configure);
    std::vector<uint8_t> packet;
    EXPECT_FALSE(encodePacket(header, &configure, packet));

}

TEST(LCLProtocolTest, FramePresentedRoundTripsWithoutFileDescriptor) {
    LCLMsgFramePresented presented{};
    presented.surfaceId = 9;
    presented.configureSerial = 11;
    presented.frameSerial = 13;
    presented.geometryGeneration = 17;
    presented.displaySequence = 19;
    presented.timestampNs = 123456789;
    presented.refreshIntervalNs = 6944444;
    presented.clientFrameStartNs = 100000000;
    presented.clientSubmitNs = 105000000;
    presented.rasterStartNs = 107000000;
    presented.rasterReadyNs = 111000000;
    presented.composeStartNs = 115000000;
    LCLHeader header{};
    header.opcode = LCLOpcode::FramePresented;
    header.payloadSize = sizeof(presented);
    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, &presented, packet));
    LCLHeader decodedHeader{};
    std::vector<uint8_t> payload;
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decodedHeader, payload));
    const auto* decoded = reinterpret_cast<const LCLMsgFramePresented*>(payload.data());
    EXPECT_EQ(decoded->configureSerial, presented.configureSerial);
    EXPECT_EQ(decoded->frameSerial, presented.frameSerial);
    EXPECT_EQ(decoded->geometryGeneration, presented.geometryGeneration);
    EXPECT_EQ(decoded->displaySequence, presented.displaySequence);
    EXPECT_EQ(decoded->timestampNs, presented.timestampNs);
    EXPECT_EQ(decoded->refreshIntervalNs, presented.refreshIntervalNs);
    EXPECT_EQ(decoded->clientFrameStartNs, presented.clientFrameStartNs);
    EXPECT_EQ(decoded->clientSubmitNs, presented.clientSubmitNs);
    EXPECT_EQ(decoded->rasterStartNs, presented.rasterStartNs);
    EXPECT_EQ(decoded->rasterReadyNs, presented.rasterReadyNs);
    EXPECT_EQ(decoded->composeStartNs, presented.composeStartNs);

    const int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);
    EXPECT_FALSE(sendMsgWithFd(STDOUT_FILENO, header, &presented, descriptor));
    close(descriptor);
}

TEST(LCLProtocolTest, SendAndReceiveSetWindowLayerMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLHeader headerSend{};
    headerSend.opcode = LCLOpcode::SetWindowLayer;

    LCLMsgSetWindowLayer msg{};
    msg.surfaceId = 1;
    msg.layer = LCLWindowLayer::Bottom;
    msg.unfocusable = 1;

    headerSend.payloadSize = sizeof(msg);

    EXPECT_TRUE(sendMsgWithFd(sv[0], headerSend, &msg, -1));

    LCLHeader headerRecv{};
    std::vector<uint8_t> payloadRecv;
    int receivedFd = -1;

    EXPECT_TRUE(recvMsgWithFd(sv[1], headerRecv, payloadRecv, receivedFd));
    EXPECT_EQ(headerRecv.opcode, LCLOpcode::SetWindowLayer);
    ASSERT_EQ(payloadRecv.size(), sizeof(LCLMsgSetWindowLayer));

    const auto* msgRecv = reinterpret_cast<const LCLMsgSetWindowLayer*>(payloadRecv.data());
    EXPECT_EQ(msgRecv->surfaceId, 1u);
    EXPECT_EQ(msgRecv->layer, LCLWindowLayer::Bottom);
    EXPECT_EQ(msgRecv->unfocusable, 1);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveSetReservedZoneMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLHeader headerSend{};
    headerSend.opcode = LCLOpcode::SetReservedZone;

    LCLMsgSetReservedZone msg{};
    msg.surfaceId = 1;
    msg.top = 32;
    msg.bottom = 60;
    msg.left = 0;
    msg.right = 0;

    headerSend.payloadSize = sizeof(msg);

    EXPECT_TRUE(sendMsgWithFd(sv[0], headerSend, &msg, -1));

    LCLHeader headerRecv{};
    std::vector<uint8_t> payloadRecv;
    int receivedFd = -1;

    EXPECT_TRUE(recvMsgWithFd(sv[1], headerRecv, payloadRecv, receivedFd));
    EXPECT_EQ(headerRecv.opcode, LCLOpcode::SetReservedZone);
    ASSERT_EQ(payloadRecv.size(), sizeof(LCLMsgSetReservedZone));

    const auto* msgRecv = reinterpret_cast<const LCLMsgSetReservedZone*>(payloadRecv.data());
    EXPECT_EQ(msgRecv->surfaceId, 1u);
    EXPECT_EQ(msgRecv->top, 32u);
    EXPECT_EQ(msgRecv->bottom, 60u);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveWindowActionMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLHeader headerSend{};
    headerSend.opcode = LCLOpcode::RequestWindowAction;
    headerSend.payloadSize = sizeof(LCLMsgRequestWindowAction);

    LCLMsgRequestWindowAction msg{};
    msg.surfaceId = 7;
    msg.action = LCLWindowAction::BeginDrag;
    msg.localX = 42.5f;
    msg.localY = 13.0f;

    ASSERT_TRUE(sendMsgWithFd(sv[0], headerSend, &msg, -1));

    LCLHeader headerRecv{};
    std::vector<uint8_t> payloadRecv;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sv[1], headerRecv, payloadRecv, receivedFd));

    EXPECT_EQ(headerRecv.opcode, LCLOpcode::RequestWindowAction);
    ASSERT_EQ(payloadRecv.size(), sizeof(LCLMsgRequestWindowAction));
    const auto* received = reinterpret_cast<const LCLMsgRequestWindowAction*>(payloadRecv.data());
    EXPECT_EQ(received->surfaceId, 7u);
    EXPECT_EQ(received->action, LCLWindowAction::BeginDrag);
    EXPECT_FLOAT_EQ(received->localX, 42.5f);
    EXPECT_FLOAT_EQ(received->localY, 13.0f);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveManagedWindowActionMsg) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    LCLMsgRequestManagedWindowAction action{};
    action.targetWindowId = 42;
    action.action = LCLWindowAction::BeginDrag;
    action.localX = 88.5f;
    action.localY = 14.0f;
    LCLHeader header{};
    header.opcode = LCLOpcode::RequestManagedWindowAction;
    header.payloadSize = sizeof(action);
    ASSERT_TRUE(sendMsgWithFd(sockets[0], header, &action));

    LCLHeader received{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sockets[1], received, payload, receivedFd));
    ASSERT_EQ(received.opcode, LCLOpcode::RequestManagedWindowAction);
    ASSERT_EQ(payload.size(), sizeof(LCLMsgRequestManagedWindowAction));
    const auto* decoded = reinterpret_cast<const
        LCLMsgRequestManagedWindowAction*>(payload.data());
    EXPECT_EQ(decoded->targetWindowId, 42u);
    EXPECT_EQ(decoded->action, LCLWindowAction::BeginDrag);
    EXPECT_FLOAT_EQ(decoded->localX, 88.5f);
    EXPECT_FLOAT_EQ(decoded->localY, 14.0f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(LCLProtocolTest, SendAndReceiveSetEffectGraphMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    std::vector<FilterOp> filters = {
        { FilterType::Blur, 15.0f },
        { FilterType::Glass, 0.6f, static_cast<uint8_t>(GlassProfile::Dense) },
        { FilterType::Saturation, 1.4f },
        { FilterType::Brightness, 1.1f },
        { FilterType::Tint, 0.5f }
    };
    filters[1].params[0] = 20.0f;
    filters[1].params[1] = 1.40f;
    filters[1].params[2] = 7.0f;
    filters[4].params[0] = 15.0f;
    filters[4].params[1] = 23.0f;
    filters[4].params[2] = 42.0f;

    EffectRegion region{};
    region.x = 10.25f;
    region.y = 20.5f;
    region.width = 300.75f;
    region.height = 180.125f;
    region.cornerRadius = 14.0f;
    region.cornerRoundness = 3.2f;
    region.boundsPolicy = EffectBoundsPolicy::OuterSurface;
    region.source = EffectSourceType::SurfaceBackdrop;
    region.blendMode = EffectBlendMode::Normal;
    region.filterCount = static_cast<uint16_t>(filters.size());
    region.filterOffset = 0;
    region.opacity = 0.85f;

    LCLMsgSetEffectGraphHeader graph{};
    graph.surfaceId = 2;
    graph.regionCount = 1;
    graph.filterCount = static_cast<uint32_t>(filters.size());

    LCLHeader headerSend{};
    headerSend.opcode = LCLOpcode::SetEffectGraph;
    headerSend.payloadSize = sizeof(LCLMsgSetEffectGraphHeader) + sizeof(EffectRegion) + filters.size() * sizeof(FilterOp);

    std::vector<uint8_t> payloadSend(headerSend.payloadSize);
    uint8_t* dst = payloadSend.data();
    std::memcpy(dst, &graph, sizeof(graph));
    dst += sizeof(graph);
    std::memcpy(dst, &region, sizeof(region));
    dst += sizeof(region);
    std::memcpy(dst, filters.data(), filters.size() * sizeof(FilterOp));

    EXPECT_TRUE(sendMsgWithFd(sv[0], headerSend, payloadSend.data(), -1));

    LCLHeader headerRecv{};
    std::vector<uint8_t> payloadRecv;
    int receivedFd = -1;

    EXPECT_TRUE(recvMsgWithFd(sv[1], headerRecv, payloadRecv, receivedFd));
    EXPECT_EQ(headerRecv.opcode, LCLOpcode::SetEffectGraph);
    ASSERT_EQ(payloadRecv.size(), headerSend.payloadSize);

    const auto* graphRecv = reinterpret_cast<const LCLMsgSetEffectGraphHeader*>(payloadRecv.data());
    EXPECT_EQ(graphRecv->surfaceId, 2u);
    EXPECT_EQ(graphRecv->regionCount, 1u);
    EXPECT_EQ(graphRecv->filterCount, 5u);

    const auto* regionRecv = reinterpret_cast<const EffectRegion*>(
        payloadRecv.data() + sizeof(LCLMsgSetEffectGraphHeader));
    EXPECT_FLOAT_EQ(regionRecv->x, 10.25f);
    EXPECT_FLOAT_EQ(regionRecv->y, 20.5f);
    EXPECT_FLOAT_EQ(regionRecv->width, 300.75f);
    EXPECT_FLOAT_EQ(regionRecv->height, 180.125f);
    EXPECT_FLOAT_EQ(regionRecv->cornerRadius, 14.0f);
    EXPECT_FLOAT_EQ(regionRecv->cornerRoundness, 3.2f);
    EXPECT_EQ(regionRecv->boundsPolicy, EffectBoundsPolicy::OuterSurface);
    EXPECT_EQ(regionRecv->source, EffectSourceType::SurfaceBackdrop);
    EXPECT_EQ(regionRecv->blendMode, EffectBlendMode::Normal);
    EXPECT_EQ(regionRecv->filterCount, 5u);
    EXPECT_EQ(regionRecv->filterOffset, 0u);
    EXPECT_FLOAT_EQ(regionRecv->opacity, 0.85f);

    const auto* opsRecv = reinterpret_cast<const FilterOp*>(payloadRecv.data() + sizeof(LCLMsgSetEffectGraphHeader) + sizeof(EffectRegion));
    EXPECT_EQ(opsRecv[0].type, FilterType::Blur);
    EXPECT_FLOAT_EQ(opsRecv[0].value, 15.0f);
    EXPECT_EQ(opsRecv[1].type, FilterType::Glass);
    EXPECT_FLOAT_EQ(opsRecv[1].value, 0.6f);
    EXPECT_EQ(static_cast<GlassProfile>(opsRecv[1].profile), GlassProfile::Dense);
    EXPECT_FLOAT_EQ(opsRecv[1].params[0], 20.0f);
    EXPECT_FLOAT_EQ(opsRecv[1].params[1], 1.40f);
    EXPECT_FLOAT_EQ(opsRecv[1].params[2], 7.0f);
    EXPECT_EQ(opsRecv[2].type, FilterType::Saturation);
    EXPECT_FLOAT_EQ(opsRecv[2].value, 1.4f);
    EXPECT_EQ(opsRecv[3].type, FilterType::Brightness);
    EXPECT_FLOAT_EQ(opsRecv[3].value, 1.1f);
    EXPECT_EQ(opsRecv[4].type, FilterType::Tint);
    EXPECT_FLOAT_EQ(opsRecv[4].value, 0.5f);
    EXPECT_FLOAT_EQ(opsRecv[4].params[0], 15.0f);
    EXPECT_FLOAT_EQ(opsRecv[4].params[1], 23.0f);
    EXPECT_FLOAT_EQ(opsRecv[4].params[2], 42.0f);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveWindowCornerStyle) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLMsgSetWindowCornerStyle sent{};
    sent.surfaceId = 7;
    sent.radius = 20.0f;
    sent.roundness = 3.2f;
    LCLHeader header{};
    header.opcode = LCLOpcode::SetWindowCornerStyle;
    header.payloadSize = sizeof(sent);

    ASSERT_TRUE(sendMsgWithFd(sv[0], header, &sent, -1));

    LCLHeader receivedHeader{};
    std::vector<uint8_t> receivedPayload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sv[1], receivedHeader, receivedPayload, receivedFd));
    EXPECT_EQ(receivedHeader.opcode, LCLOpcode::SetWindowCornerStyle);
    ASSERT_EQ(receivedPayload.size(), sizeof(LCLMsgSetWindowCornerStyle));
    const auto* received = reinterpret_cast<const LCLMsgSetWindowCornerStyle*>(receivedPayload.data());
    EXPECT_EQ(received->surfaceId, 7u);
    EXPECT_FLOAT_EQ(received->radius, 20.0f);
    EXPECT_FLOAT_EQ(received->roundness, 3.2f);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveEdgeToEdge) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    LCLMsgSetEdgeToEdge sent{};
    sent.surfaceId = 7;
    sent.enabled = 1;
    LCLHeader header{};
    header.opcode = LCLOpcode::SetEdgeToEdge;
    header.payloadSize = sizeof(sent);

    ASSERT_TRUE(sendMsgWithFd(sv[0], header, &sent, -1));

    LCLHeader receivedHeader{};
    std::vector<uint8_t> receivedPayload;
    int receivedFd = -1;
    ASSERT_TRUE(recvMsgWithFd(sv[1], receivedHeader, receivedPayload, receivedFd));
    EXPECT_EQ(receivedHeader.opcode, LCLOpcode::SetEdgeToEdge);
    ASSERT_EQ(receivedPayload.size(), sizeof(LCLMsgSetEdgeToEdge));
    const auto* received =
        reinterpret_cast<const LCLMsgSetEdgeToEdge*>(receivedPayload.data());
    EXPECT_EQ(received->surfaceId, 7u);
    EXPECT_EQ(received->enabled, 1u);

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, ShellStateSnapshotAndDeltaRoundTripWithExplicitRevision) {
    LCLMsgShellStateSnapshot snapshot{};
    snapshot.revision = 0x0102030405060708ull;
    snapshot.sceneCount = 1;
    snapshot.seatId = 2;
    snapshot.displayId = 3;
    snapshot.workspaceId = 4;
    snapshot.activeSceneId = 42;
    LCLMsgShellScene scene{};
    scene.sceneId = 42;
    scene.appInstanceId = 77;
    scene.windowId = 9;
    scene.clientPid = 1234;
    scene.displayId = 3;
    scene.workspaceId = 4;
    scene.x = 80;
    scene.y = 60;
    scene.width = 540;
    scene.height = 360;
    scene.visibility = LCLSceneVisibility::Visible;
    scene.decorationMode = LCLDecorationMode::CSD;
    scene.edgeToEdge = 1;
    std::strncpy(scene.appId, "org.lcl.terminal", sizeof(scene.appId) - 1);
    std::strncpy(scene.title, "LCL Terminal", sizeof(scene.title) - 1);

    std::vector<uint8_t> native(sizeof(snapshot) + sizeof(scene));
    std::memcpy(native.data(), &snapshot, sizeof(snapshot));
    std::memcpy(native.data() + sizeof(snapshot), &scene, sizeof(scene));
    LCLHeader header{};
    header.opcode = LCLOpcode::ShellStateSnapshot;
    header.requestId = 41;
    header.payloadSize = static_cast<uint32_t>(native.size());
    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, native.data(), packet));
    EXPECT_EQ(packet[24], 0x08); // revision is always little-endian on the wire.

    LCLHeader decodedHeader{};
    std::vector<uint8_t> decoded;
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decodedHeader, decoded));
    ASSERT_EQ(decodedHeader.opcode, LCLOpcode::ShellStateSnapshot);
    ASSERT_EQ(decoded.size(), native.size());
    const auto* decodedSnapshot = reinterpret_cast<const LCLMsgShellStateSnapshot*>(decoded.data());
    const auto* decodedScene = reinterpret_cast<const LCLMsgShellScene*>(decoded.data() + sizeof(*decodedSnapshot));
    EXPECT_EQ(decodedSnapshot->revision, snapshot.revision);
    EXPECT_EQ(decodedSnapshot->activeSceneId, 42u);
    EXPECT_EQ(decodedScene->sceneId, 42u);
    EXPECT_EQ(decodedScene->decorationMode, LCLDecorationMode::CSD);
    EXPECT_EQ(decodedScene->edgeToEdge, 1u);
    EXPECT_STREQ(decodedScene->appId, "org.lcl.terminal");

    LCLMsgShellStateDelta delta{};
    delta.revision = snapshot.revision + 1;
    delta.kind = LCLShellStateDeltaKind::FocusChanged;
    delta.seatId = 2;
    delta.displayId = 3;
    delta.workspaceId = 4;
    delta.activeSceneId = 42;
    header.opcode = LCLOpcode::ShellStateDelta;
    header.requestId = 42;
    header.payloadSize = sizeof(delta);
    ASSERT_TRUE(encodePacket(header, &delta, packet));
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decodedHeader, decoded));
    ASSERT_EQ(decoded.size(), sizeof(delta));
    const auto* decodedDelta = reinterpret_cast<const LCLMsgShellStateDelta*>(decoded.data());
    EXPECT_EQ(decodedDelta->kind, LCLShellStateDeltaKind::FocusChanged);
    EXPECT_EQ(decodedDelta->activeSceneId, 42u);
}

TEST(LCLProtocolTest, SystemSurfaceDeclarationRoundTripsAndRejectsNone) {
    LCLMsgSetSystemSurfaceKind request{};
    request.kind = LCLSystemSurfaceKind::HomeScreen;
    LCLHeader header{};
    header.opcode = LCLOpcode::SetSystemSurfaceKind;
    header.requestId = 63;
    header.payloadSize = sizeof(request);
    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, &request, packet));

    LCLHeader decodedHeader{};
    std::vector<uint8_t> payload;
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decodedHeader, payload));
    ASSERT_EQ(decodedHeader.opcode, LCLOpcode::SetSystemSurfaceKind);
    ASSERT_EQ(payload.size(), sizeof(request));
    EXPECT_EQ(reinterpret_cast<const LCLMsgSetSystemSurfaceKind*>(payload.data())->kind,
              LCLSystemSurfaceKind::HomeScreen);

    request.kind = LCLSystemSurfaceKind::None;
    EXPECT_FALSE(encodePacket(header, &request, packet));
}

TEST(LCLProtocolTest, LaunchPlaceholderCommandsRoundTrip) {
    LCLMsgBeginLaunchPlaceholder begin{};
    begin.homeSurfaceId = 1;
    begin.launchToken = 73;
    std::strncpy(begin.appId, "org.lcl.test", sizeof(begin.appId) - 1);
    begin.originX = 24.0f;
    begin.originY = 32.0f;
    begin.originWidth = 60.0f;
    begin.originHeight = 60.0f;
    begin.originCornerRadius = 14.0f;
    begin.iconWidth = 2;
    begin.iconHeight = 2;
    const std::array<uint32_t, 4> iconPixels{
        0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    std::vector<uint8_t> beginPayload(
        sizeof(begin) + sizeof(iconPixels));
    std::memcpy(beginPayload.data(), &begin, sizeof(begin));
    std::memcpy(beginPayload.data() + sizeof(begin),
                iconPixels.data(), sizeof(iconPixels));

    LCLHeader header{};
    header.opcode = LCLOpcode::BeginLaunchPlaceholder;
    header.requestId = 71;
    header.payloadSize = static_cast<uint32_t>(beginPayload.size());
    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, beginPayload.data(), packet));
    LCLHeader decodedHeader{};
    std::vector<uint8_t> payload;
    ASSERT_TRUE(decodePacket(
        packet.data(), packet.size(), decodedHeader, payload));
    ASSERT_EQ(payload.size(), beginPayload.size());
    const auto* decodedBegin = reinterpret_cast<const
        LCLMsgBeginLaunchPlaceholder*>(payload.data());
    EXPECT_EQ(decodedBegin->launchToken, 73u);
    EXPECT_STREQ(decodedBegin->appId, "org.lcl.test");
    EXPECT_EQ(decodedBegin->iconWidth, 2u);
    EXPECT_EQ(decodedBegin->iconHeight, 2u);
    EXPECT_EQ(0, std::memcmp(payload.data() + sizeof(*decodedBegin),
                             iconPixels.data(), sizeof(iconPixels)));

    LCLMsgResolveLaunchPlaceholder resolve{};
    resolve.homeSurfaceId = 1;
    resolve.launchToken = 73;
    resolve.appInstanceId = 91;
    resolve.reused = 1;
    header.opcode = LCLOpcode::ResolveLaunchPlaceholder;
    header.payloadSize = sizeof(resolve);
    ASSERT_TRUE(encodePacket(header, &resolve, packet));
    ASSERT_TRUE(decodePacket(
        packet.data(), packet.size(), decodedHeader, payload));
    const auto* decodedResolve = reinterpret_cast<const
        LCLMsgResolveLaunchPlaceholder*>(payload.data());
    EXPECT_EQ(decodedResolve->appInstanceId, 91u);
    EXPECT_EQ(decodedResolve->reused, 1u);

    LCLMsgCancelLaunchPlaceholder cancel{};
    cancel.homeSurfaceId = 1;
    cancel.launchToken = 73;
    header.opcode = LCLOpcode::CancelLaunchPlaceholder;
    header.payloadSize = sizeof(cancel);
    ASSERT_TRUE(encodePacket(header, &cancel, packet));
    EXPECT_TRUE(decodePacket(
        packet.data(), packet.size(), decodedHeader, payload));

    LCLMsgLaunchIconVisibility visibility{};
    visibility.launchToken = 73;
    std::strncpy(visibility.appId, "org.lcl.test",
                 sizeof(visibility.appId) - 1);
    visibility.visible = 1;
    header.opcode = LCLOpcode::LaunchIconVisibility;
    header.payloadSize = sizeof(visibility);
    ASSERT_TRUE(encodePacket(header, &visibility, packet));
    ASSERT_TRUE(decodePacket(
        packet.data(), packet.size(), decodedHeader, payload));
    ASSERT_EQ(payload.size(), sizeof(visibility));
    const auto* decodedVisibility = reinterpret_cast<const
        LCLMsgLaunchIconVisibility*>(payload.data());
    EXPECT_EQ(decodedVisibility->launchToken, 73u);
    EXPECT_STREQ(decodedVisibility->appId, "org.lcl.test");
    EXPECT_EQ(decodedVisibility->visible, 1u);

    LCLMsgLaunchIconVisibilityAck visibilityAck{};
    visibilityAck.launchToken = 73;
    std::strncpy(visibilityAck.appId, "org.lcl.test",
                 sizeof(visibilityAck.appId) - 1);
    header.opcode = LCLOpcode::LaunchIconVisibilityAck;
    header.payloadSize = sizeof(visibilityAck);
    ASSERT_TRUE(encodePacket(header, &visibilityAck, packet));
    ASSERT_TRUE(decodePacket(
        packet.data(), packet.size(), decodedHeader, payload));
    ASSERT_EQ(payload.size(), sizeof(visibilityAck));
    const auto* decodedVisibilityAck = reinterpret_cast<const
        LCLMsgLaunchIconVisibilityAck*>(payload.data());
    EXPECT_EQ(decodedVisibilityAck->launchToken, 73u);
    EXPECT_STREQ(decodedVisibilityAck->appId, "org.lcl.test");

}

TEST(LCLProtocolTest, SurfaceCreateRequiresCanonicalAppId) {
    LCLMsgSurfaceCreate request{};
    request.surfaceId = 7;
    request.width = 640;
    request.height = 480;
    std::strncpy(request.title, "No identity", sizeof(request.title) - 1);

    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceCreate;
    header.requestId = 64;
    header.payloadSize = sizeof(request);
    std::vector<uint8_t> packet;
    EXPECT_FALSE(encodePacket(header, &request, packet));

    std::strncpy(request.appId, "org.lcl.test", sizeof(request.appId) - 1);
    EXPECT_TRUE(encodePacket(header, &request, packet));
}

TEST(LCLProtocolTest, SurfaceCreateRejectsInvalidResizeConstraints) {
    LCLMsgSurfaceCreate request{};
    request.surfaceId = 7;
    request.width = 640;
    request.height = 480;
    std::strncpy(request.title, "Resize constraints", sizeof(request.title) - 1);
    std::strncpy(request.appId, "org.lcl.resize-test", sizeof(request.appId) - 1);

    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceCreate;
    header.payloadSize = sizeof(request);
    std::vector<uint8_t> packet;

    request.resizeWidthIncrement = -1.0f;
    EXPECT_FALSE(encodePacket(header, &request, packet));

    request.resizeWidthIncrement =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(encodePacket(header, &request, packet));

    request.resizeWidthIncrement = 8.0f;
    request.resizeHeightIncrement = 16.0f;
    EXPECT_TRUE(encodePacket(header, &request, packet));
}

TEST(LCLProtocolTest, InputEventCodecRoundTripWithPointerIdentityAndCancel) {
    LCLMsgInputEvent input{};
    input.surfaceId = 12;
    input.type = static_cast<uint32_t>(LCLInputEventType::PointerCancel);
    input.key = 1;
    input.pressed = 1;
    input.modifiers = 0x05;
    input.source = static_cast<uint8_t>(LCLPointerSource::Touch);
    input.pointerId = 42;
    input.codepoint = 0;
    input.x = 123.45f;
    input.y = 678.90f;
    input.deltaX = -1.5f;
    input.deltaY = 2.5f;

    LCLHeader header{};
    header.opcode = LCLOpcode::InputEvent;
    header.requestId = 88;
    header.payloadSize = sizeof(input);

    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, &input, packet));

    LCLHeader decodedHeader{};
    std::vector<uint8_t> decodedPayload;
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decodedHeader, decodedPayload));

    EXPECT_EQ(decodedHeader.opcode, LCLOpcode::InputEvent);
    EXPECT_EQ(decodedHeader.requestId, 88u);
    ASSERT_EQ(decodedPayload.size(), sizeof(input));

    const auto* decodedInput = reinterpret_cast<const LCLMsgInputEvent*>(decodedPayload.data());
    EXPECT_EQ(decodedInput->surfaceId, 12u);
    EXPECT_EQ(decodedInput->type,
              static_cast<uint32_t>(LCLInputEventType::PointerCancel));
    EXPECT_EQ(decodedInput->key, 1u);
    EXPECT_EQ(decodedInput->pressed, 1u);
    EXPECT_EQ(decodedInput->modifiers, 0x05u);
    EXPECT_EQ(decodedInput->source, static_cast<uint8_t>(LCLPointerSource::Touch));
    EXPECT_EQ(decodedInput->pointerId, 42u);
    EXPECT_FLOAT_EQ(decodedInput->x, 123.45f);
    EXPECT_FLOAT_EQ(decodedInput->y, 678.90f);
    EXPECT_FLOAT_EQ(decodedInput->deltaX, -1.5f);
    EXPECT_FLOAT_EQ(decodedInput->deltaY, 2.5f);
}
