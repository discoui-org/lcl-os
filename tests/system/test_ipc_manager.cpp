#include <gtest/gtest.h>
#include "system/ipc/ipc_manager.hpp"
#include "system/security/session_user.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include <filesystem>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace lcl::core;
using namespace lcl::protocol;

class IPCManagerTest : public ::testing::Test {
protected:
    std::string testSocketPath;

    void SetUp() override {
        testSocketPath = (fs::temp_directory_path() / "test_lcl_compositor.sock").string();
        unlink(testSocketPath.c_str());
    }

    void TearDown() override {
        unlink(testSocketPath.c_str());
    }
};

TEST_F(IPCManagerTest, InitializeSocketServerAndPermissions) {
    IPCManager manager;
    ASSERT_TRUE(manager.initialize(testSocketPath));
    EXPECT_TRUE(manager.isInitialized());

    // Verify socket file exists
    struct stat st{};
    ASSERT_EQ(stat(testSocketPath.c_str(), &st), 0);

    // Session and sandbox applications share only this graphics endpoint.
    mode_t perm = st.st_mode & 0777;
    EXPECT_EQ(perm, static_cast<mode_t>(0660));
    if (geteuid() == 0) {
        EXPECT_EQ(st.st_uid, lcl::security::kSessionUserUid);
        EXPECT_EQ(st.st_gid, lcl::security::kApplicationRuntimeGid);
    }

    manager.shutdown();
    EXPECT_FALSE(manager.isInitialized());
}
TEST_F(IPCManagerTest, ClientConnectionAndPeerCreds) {
    IPCManager manager;
    ASSERT_TRUE(manager.initialize(testSocketPath));

    // Connect a client socket
    int clientFd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    ASSERT_GE(clientFd, 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, testSocketPath.c_str(), sizeof(addr.sun_path) - 1);

    ASSERT_EQ(connect(clientFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

    // Send a binary protocol message
    LCLHeader header{};
    header.magic = LCL_PROTOCOL_MAGIC;
    header.version = LCL_PROTOCOL_VERSION;
    header.opcode = LCLOpcode::SurfaceDestroy;
    header.requestId = 1;

    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 71;
    header.payloadSize = sizeof(destroy);

    ASSERT_TRUE(sendMsgWithFd(clientFd, header, &destroy, -1));

    // Poll messages on server
    auto msgs = manager.pollMessages();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].pid, getpid());
    EXPECT_EQ(msgs[0].uid, getuid());
    EXPECT_EQ(msgs[0].gid, getgid());
    EXPECT_FALSE(msgs[0].disconnected);
    EXPECT_EQ(msgs[0].header.opcode, LCLOpcode::SurfaceDestroy);
    ASSERT_EQ(msgs[0].payload.size(), sizeof(LCLMsgSurfaceDestroy));
    LCLMsgSurfaceDestroy received{};
    std::memcpy(&received, msgs[0].payload.data(), sizeof(received));
    EXPECT_EQ(received.surfaceId, 71u);

    close(clientFd);
    manager.shutdown();
}

TEST_F(IPCManagerTest, RejectsClientRequestWithoutRequestId) {
    IPCManager manager;
    ASSERT_TRUE(manager.initialize(testSocketPath));

    int clientFd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    ASSERT_GE(clientFd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, testSocketPath.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(connect(clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    LCLMsgSurfaceDestroy destroy{};
    destroy.surfaceId = 72;
    LCLHeader header{};
    header.opcode = LCLOpcode::SurfaceDestroy;
    header.payloadSize = sizeof(destroy);
    ASSERT_TRUE(sendMsgWithFd(clientFd, header, &destroy));

    const auto messages = manager.pollMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_TRUE(messages.front().disconnected);

    close(clientFd);
    manager.shutdown();
}
