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
