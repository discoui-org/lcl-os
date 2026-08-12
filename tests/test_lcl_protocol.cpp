#include <gtest/gtest.h>
#include "core/ipc/lcl_protocol.hpp"
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
    msg.bufferScale = 1.5f;
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
    EXPECT_EQ(msgRecv->x, 100);
    EXPECT_EQ(msgRecv->y, 200);
    EXPECT_EQ(msgRecv->width, 800u);
    EXPECT_EQ(msgRecv->height, 600u);
    EXPECT_FLOAT_EQ(msgRecv->bufferScale, 1.5f);
    EXPECT_STREQ(msgRecv->title, "Test Window Title");
    EXPECT_STREQ(msgRecv->appId, "org.lcl.test");

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, ConfigureAndAttachRoundTripTheSameSerial) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    LCLMsgConfigureBounds configure{};
    configure.surfaceId = 4;
    configure.configureSerial = 0x0102030405060708ull;
    configure.width = 640;
    configure.height = 480;
    configure.bufferScale = 1.0f;
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

std::vector<uint8_t> surfaceCreatePacket(float scale) {
    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceCreate;
    header.requestId = 91;
    header.payloadSize = sizeof(LCLMsgSurfaceCreate);
    LCLMsgSurfaceCreate create{};
    create.surfaceId = 4;
    create.width = 640;
    create.height = 480;
    create.bufferScale = scale;
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

TEST(LCLProtocolTest, RejectsMissingAndInvalidBufferScale) {
    auto packet = surfaceCreatePacket(1.5f);
    ASSERT_FALSE(packet.empty());

    // A v3 SurfaceCreate must contain the final float bufferScale field.
    packet.resize(packet.size() - sizeof(float));
    const uint32_t shortened = sizeof(LCLMsgSurfaceCreate) - sizeof(float);
    packet[20] = static_cast<uint8_t>(shortened);
    packet[21] = static_cast<uint8_t>(shortened >> 8);
    packet[22] = static_cast<uint8_t>(shortened >> 16);
    packet[23] = static_cast<uint8_t>(shortened >> 24);
    LCLHeader header{};
    std::vector<uint8_t> payload;
    EXPECT_FALSE(decodePacket(packet.data(), packet.size(), header, payload));

    packet = surfaceCreatePacket(1.5f);
    const uint32_t nanBits =
        std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN());
    const size_t scaleOffset = packet.size() - sizeof(float);
    packet[scaleOffset] = static_cast<uint8_t>(nanBits);
    packet[scaleOffset + 1] = static_cast<uint8_t>(nanBits >> 8);
    packet[scaleOffset + 2] = static_cast<uint8_t>(nanBits >> 16);
    packet[scaleOffset + 3] = static_cast<uint8_t>(nanBits >> 24);
    EXPECT_FALSE(decodePacket(packet.data(), packet.size(), header, payload));

    EXPECT_TRUE(surfaceCreatePacket(0.49f).empty());
    EXPECT_TRUE(surfaceCreatePacket(4.01f).empty());
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
    auto packet = surfaceCreatePacket(1.0f);
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

TEST(LCLProtocolTest, FileDescriptorIsAcceptedOnlyForAttachBuffer) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);
    int descriptor = dup(STDIN_FILENO);
    ASSERT_GE(descriptor, 0);

    LCLMsgAttachBuffer attach{};
    attach.surfaceId = 3;
    attach.configureSerial = 7;
    attach.width = 16;
    attach.height = 16;
    attach.stride = 64;
    attach.format = 1;
    LCLHeader attachHeader{};
    attachHeader.opcode = LCLOpcode::AttachBuffer;
    attachHeader.requestId = 5;
    attachHeader.payloadSize = sizeof(attach);
    ASSERT_TRUE(sendMsgWithFd(sv[0], attachHeader, &attach, descriptor));

    LCLHeader receivedHeader{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_EQ(recvPacketWithFd(sv[1], receivedHeader, payload, receivedFd),
              ReceiveStatus::Received);
    EXPECT_GE(receivedFd, 0);
    EXPECT_EQ(receivedHeader.requestId, 5u);
    if (receivedFd >= 0)
        close(receivedFd);

    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 3;
    LCLHeader destroyHeader{};
    destroyHeader.opcode = LCLOpcode::SurfaceDestroy;
    destroyHeader.payloadSize = sizeof(destroy);
    EXPECT_FALSE(sendMsgWithFd(sv[0], destroyHeader, &destroy, descriptor));

    close(descriptor);
    close(sv[0]);
    close(sv[1]);
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

TEST(LCLProtocolTest, SendAndReceiveSetEffectGraphMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv), 0);

    std::vector<FilterOp> filters = {
        { FilterType::Blur, 15.0f },
        { FilterType::Glass, 0.6f, static_cast<uint8_t>(GlassProfile::Dense) },
        { FilterType::Saturation, 1.4f },
        { FilterType::Brightness, 1.1f }
    };
    filters[1].params[0] = 20.0f;
    filters[1].params[1] = 1.40f;
    filters[1].params[2] = 7.0f;

    EffectRegion region{};
    region.x = 10;
    region.y = 20;
    region.width = 300;
    region.height = 180;
    region.cornerRadius = 14.0f;
    region.source = EffectSourceType::Backdrop;
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
    EXPECT_EQ(graphRecv->filterCount, 4u);

    const auto* regionRecv = reinterpret_cast<const EffectRegion*>(
        payloadRecv.data() + sizeof(LCLMsgSetEffectGraphHeader));
    EXPECT_EQ(regionRecv->x, 10);
    EXPECT_EQ(regionRecv->y, 20);
    EXPECT_EQ(regionRecv->width, 300u);
    EXPECT_EQ(regionRecv->height, 180u);
    EXPECT_FLOAT_EQ(regionRecv->cornerRadius, 14.0f);
    EXPECT_EQ(regionRecv->source, EffectSourceType::Backdrop);
    EXPECT_EQ(regionRecv->blendMode, EffectBlendMode::Normal);
    EXPECT_EQ(regionRecv->filterCount, 4u);
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
    request.kind = LCLSystemSurfaceKind::Dock;
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
              LCLSystemSurfaceKind::Dock);

    request.kind = LCLSystemSurfaceKind::None;
    EXPECT_FALSE(encodePacket(header, &request, packet));
}

TEST(LCLProtocolTest, SurfaceCreateRequiresCanonicalAppId) {
    LCLMsgSurfaceCreate request{};
    request.surfaceId = 7;
    request.width = 640;
    request.height = 480;
    request.bufferScale = 1.0f;
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
