#include <gtest/gtest.h>
#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
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

    // Verify permissions are 0600 (S_IRUSR | S_IWUSR)
    mode_t perm = st.st_mode & 0777;
    EXPECT_EQ(perm, static_cast<mode_t>(0600));

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
    header.opcode = LCLOpcode::RegisterRole;
    header.requestId = 1;

    LCLMsgRegisterRole reg{};
    reg.role = LCLRole::ClientApp;
    std::strncpy(reg.clientName, "UnitTestClient", sizeof(reg.clientName) - 1);
    header.payloadSize = sizeof(reg);

    ASSERT_TRUE(sendMsgWithFd(clientFd, header, &reg, -1));

    // Poll messages on server
    auto msgs = manager.pollMessages();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].pid, getpid());
    EXPECT_EQ(msgs[0].uid, getuid());
    EXPECT_EQ(msgs[0].gid, getgid());
    EXPECT_FALSE(msgs[0].disconnected);
    EXPECT_EQ(msgs[0].header.opcode, LCLOpcode::RegisterRole);
    ASSERT_EQ(msgs[0].payload.size(), sizeof(LCLMsgRegisterRole));
    LCLMsgRegisterRole received{};
    std::memcpy(&received, msgs[0].payload.data(), sizeof(received));
    EXPECT_STREQ(received.clientName, "UnitTestClient");

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

    LCLMsgRegisterRole registration{};
    registration.role = LCLRole::ClientApp;
    std::strncpy(registration.clientName, "MissingRequestId",
                 sizeof(registration.clientName) - 1);
    LCLHeader header{};
    header.opcode = LCLOpcode::RegisterRole;
    header.payloadSize = sizeof(registration);
    ASSERT_TRUE(sendMsgWithFd(clientFd, header, &registration));

    const auto messages = manager.pollMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_TRUE(messages.front().disconnected);

    close(clientFd);
    manager.shutdown();
}
