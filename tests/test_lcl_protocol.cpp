#include <gtest/gtest.h>
#include "core/ipc/lcl_protocol.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

using namespace lcl::protocol;

TEST(LCLProtocolTest, HeaderMagicAndDefaults) {
    LCLHeader header;
    EXPECT_EQ(header.magic, LCL_PROTOCOL_MAGIC);
    EXPECT_EQ(header.version, LCL_PROTOCOL_VERSION);
    EXPECT_EQ(header.opcode, LCLOpcode::AckResponse);
    EXPECT_EQ(header.payloadSize, 0u);
}

TEST(LCLProtocolTest, SendAndReceiveMsgOverSocketPair) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    LCLHeader headerSend;
    headerSend.magic = LCL_PROTOCOL_MAGIC;
    headerSend.version = LCL_PROTOCOL_VERSION;
    headerSend.opcode = LCLOpcode::SurfaceCreate;
    
    LCLMsgSurfaceCreate msg{};
    msg.surfaceId = 42;
    msg.x = 100;
    msg.y = 200;
    msg.width = 800;
    msg.height = 600;
    std::strncpy(msg.title, "Test Window Title", sizeof(msg.title) - 1);

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
    EXPECT_EQ(headerRecv.payloadSize, sizeof(LCLMsgSurfaceCreate));
    ASSERT_EQ(payloadRecv.size(), sizeof(LCLMsgSurfaceCreate));

    const auto* msgRecv = reinterpret_cast<const LCLMsgSurfaceCreate*>(payloadRecv.data());
    EXPECT_EQ(msgRecv->surfaceId, 42u);
    EXPECT_EQ(msgRecv->x, 100);
    EXPECT_EQ(msgRecv->y, 200);
    EXPECT_EQ(msgRecv->width, 800u);
    EXPECT_EQ(msgRecv->height, 600u);
    EXPECT_STREQ(msgRecv->title, "Test Window Title");

    close(sv[0]);
    close(sv[1]);
}

TEST(LCLProtocolTest, SendAndReceiveSetWindowLayerMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

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
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

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

TEST(LCLProtocolTest, SendAndReceiveSetEffectGraphMsg) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    std::vector<FilterOp> filters = {
        { FilterType::Blur, 15.0f },
        { FilterType::Saturation, 1.4f },
        { FilterType::Brightness, 1.1f }
    };

    EffectRegion region{};
    region.x = 10;
    region.y = 20;
    region.width = 300;
    region.height = 180;
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
    EXPECT_EQ(graphRecv->filterCount, 3u);

    const auto* regionRecv = reinterpret_cast<const EffectRegion*>(
        payloadRecv.data() + sizeof(LCLMsgSetEffectGraphHeader));
    EXPECT_EQ(regionRecv->x, 10);
    EXPECT_EQ(regionRecv->y, 20);
    EXPECT_EQ(regionRecv->width, 300u);
    EXPECT_EQ(regionRecv->height, 180u);
    EXPECT_EQ(regionRecv->source, EffectSourceType::Backdrop);
    EXPECT_EQ(regionRecv->blendMode, EffectBlendMode::Normal);
    EXPECT_EQ(regionRecv->filterCount, 3u);
    EXPECT_EQ(regionRecv->filterOffset, 0u);
    EXPECT_FLOAT_EQ(regionRecv->opacity, 0.85f);

    const auto* opsRecv = reinterpret_cast<const FilterOp*>(payloadRecv.data() + sizeof(LCLMsgSetEffectGraphHeader) + sizeof(EffectRegion));
    EXPECT_EQ(opsRecv[0].type, FilterType::Blur);
    EXPECT_FLOAT_EQ(opsRecv[0].value, 15.0f);
    EXPECT_EQ(opsRecv[1].type, FilterType::Saturation);
    EXPECT_FLOAT_EQ(opsRecv[1].value, 1.4f);
    EXPECT_EQ(opsRecv[2].type, FilterType::Brightness);
    EXPECT_FLOAT_EQ(opsRecv[2].value, 1.1f);

    close(sv[0]);
    close(sv[1]);
}
