#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "system/session/app_registry.hpp"
#include "system/session/session_protocol.hpp"
#include "system/session/session_client.hpp"
#include "system/session/session_service.hpp"

namespace fs = std::filesystem;
using namespace lcl::session;

class SessionServiceTest : public ::testing::Test {
protected:
    fs::path tempDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() /
            ("lcl-session-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tempDir);
    }
    void TearDown() override { fs::remove_all(tempDir); }

    fs::path createBundle(const std::string& directory, const std::string& appId,
                          const std::string& executableBody = "#!/bin/sh\nexit 23\n") {
        const fs::path bundle = tempDir / directory;
        fs::create_directories(bundle / "Executables");
        fs::create_directories(bundle / "Resources");
        std::ofstream manifest(bundle / "Manifest.json");
        manifest << "{\"id\":\"" << appId
                 << "\",\"name\":\"Test App\",\"version\":\"1.0\","
                    "\"executable\":\"Executables/test-app\",\"icon\":\"Resources/Icon.png\",\"type\":\"gui\"}";
        manifest.close();
        std::ofstream executable(bundle / "Executables" / "test-app");
        executable << executableBody;
        executable.close();
        chmod((bundle / "Executables" / "test-app").c_str(), 0755);
        return bundle;
    }
};

TEST(SessionProtocolTest, EncodesExplicitLittleEndianLaunchPacket) {
    std::vector<uint8_t> payload;
    LaunchRequest sent;
    sent.target = "org.lcl.test";
    sent.waitForExit = true;
    sent.singleInstance = true;
    sent.launchToken = 73;
    sent.origin = {true, 12.5f, 24.0f, 60.0f, 60.0f, 14.0f};
    ASSERT_TRUE(encodeLaunchRequest(sent, payload));
    SessionHeader header{};
    header.opcode = SessionOpcode::LaunchRequest;
    header.requestId = 0x44332211u;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> packet;
    ASSERT_TRUE(encodePacket(header, payload, packet));
    EXPECT_EQ(packet[12], 0x11);
    EXPECT_EQ(packet[13], 0x22);
    EXPECT_EQ(packet[14], 0x33);
    EXPECT_EQ(packet[15], 0x44);

    DecodedPacket decoded;
    ASSERT_TRUE(decodePacket(packet.data(), packet.size(), decoded));
    LaunchRequest request;
    ASSERT_TRUE(decodeLaunchRequest(decoded.payload, request));
    EXPECT_EQ(request.target, "org.lcl.test");
    EXPECT_TRUE(request.waitForExit);
    EXPECT_TRUE(request.singleInstance);
    EXPECT_EQ(request.launchToken, 73u);
    EXPECT_TRUE(request.origin.valid);
    EXPECT_FLOAT_EQ(request.origin.x, 12.5f);
    EXPECT_FLOAT_EQ(request.origin.y, 24.0f);
    EXPECT_FLOAT_EQ(request.origin.width, 60.0f);
    EXPECT_FLOAT_EQ(request.origin.height, 60.0f);
    EXPECT_FLOAT_EQ(request.origin.cornerRadius, 14.0f);
}

TEST_F(SessionServiceTest, RegistryUsesManifestIdAndFindsBundleAlias) {
    createBundle("Example.app", "org.lcl.example");
    AppRegistry registry({tempDir.string()});
    registry.refresh();
    ASSERT_EQ(registry.entries().size(), 1u);
    EXPECT_EQ(registry.entries().front().appId, "org.lcl.example");
    EXPECT_TRUE(registry.find("org.lcl.example").has_value());
    EXPECT_TRUE(registry.find("Example.app").has_value());
    EXPECT_TRUE(registry.find((tempDir / "Example.app").string()).has_value());
}

TEST_F(SessionServiceTest, RegistryKeepsFirstBundleForDuplicateCanonicalId) {
    const fs::path first = createBundle("HigherPriority/First.app", "org.lcl.duplicate");
    createBundle("LowerPriority/Second.app", "org.lcl.duplicate");

    AppRegistry registry({(tempDir / "HigherPriority").string(),
                          (tempDir / "LowerPriority").string()});
    registry.refresh();

    ASSERT_EQ(registry.entries().size(), 1u);
    EXPECT_EQ(registry.entries().front().appId, "org.lcl.duplicate");
    EXPECT_EQ(registry.entries().front().bundlePath, first.string());
}

TEST_F(SessionServiceTest, ServiceOwnsLaunchAndExitLifecycle) {
    createBundle("Lifecycle.app", "org.lcl.lifecycle");
    SessionService service({tempDir.string()});
    service.refreshCatalog();
    const LaunchResponse launch = service.launch({"org.lcl.lifecycle", false});
    ASSERT_EQ(launch.status, 0u) << launch.message;
    ASSERT_GT(launch.instanceId, 0u);
    ASSERT_GT(launch.pid, 0);

    for (int i = 0; i < 100 && service.instances().at(launch.instanceId).running; ++i) {
        service.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto& instance = service.instances().at(launch.instanceId);
    EXPECT_FALSE(instance.running);
    EXPECT_EQ(instance.exitCode, 23);
    EXPECT_EQ(instance.appId, "org.lcl.lifecycle");
}

TEST_F(SessionServiceTest, ServiceLaunchesTheVerifiedExecutableInode) {
    const fs::path bundle = createBundle("Pinned.app", "org.lcl.pinned",
                                         "#!/bin/sh\nexit 23\n");
    SessionService service({tempDir.string()});
    service.refreshCatalog();

    const fs::path executable = bundle / "Executables" / "test-app";
    const fs::path verifiedExecutable = executable.string() + ".verified";
    fs::rename(executable, verifiedExecutable);
    std::ofstream replacement(executable);
    replacement << "#!/bin/sh\nexit 99\n";
    replacement.close();
    chmod(executable.c_str(), 0755);

    const LaunchResponse launch = service.launch({"org.lcl.pinned", false});
    ASSERT_EQ(launch.status, 0u) << launch.message;
    for (int index = 0; index < 100 && service.instances().at(launch.instanceId).running; ++index) {
        service.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(service.instances().at(launch.instanceId).exitCode, 23);
}

TEST_F(SessionServiceTest, SingleInstanceLaunchReusesRunningProcess) {
    createBundle("Singleton.app", "org.lcl.singleton",
                 "#!/bin/sh\nsleep 1\nexit 0\n");
    SessionService service({tempDir.string()});
    service.refreshCatalog();

    LaunchRequest request;
    request.target = "org.lcl.singleton";
    request.singleInstance = true;
    const LaunchResponse first = service.launch(request);
    ASSERT_EQ(first.status, 0u) << first.message;
    EXPECT_FALSE(first.reused);

    const LaunchResponse second = service.launch(request);
    ASSERT_EQ(second.status, 0u) << second.message;
    EXPECT_TRUE(second.reused);
    EXPECT_EQ(second.instanceId, first.instanceId);
    EXPECT_EQ(second.pid, first.pid);
    EXPECT_EQ(service.instances().size(), 1u);
}

TEST_F(SessionServiceTest, ClientUsesSessiondForLaunchAndWait) {
    createBundle("Rpc.app", "org.lcl.rpc", "#!/bin/sh\nsleep 0.02\nexit 9\n");
    SessionService service({tempDir.string()});
    const std::string socketPath = (tempDir / "sessiond.sock").string();
    ASSERT_TRUE(service.initialize(socketPath));

    std::atomic<bool> serving{true};
    std::thread server([&] {
        while (serving.load()) {
            service.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    SessionClient client;
    ASSERT_TRUE(client.connect(socketPath));
    LaunchResponse response;
    std::string error;
    ASSERT_TRUE(client.launch({"org.lcl.rpc", true}, response, error)) << error;
    ProcessExited exited;
    ASSERT_TRUE(client.waitForExit(response.instanceId, exited, error)) << error;
    EXPECT_EQ(exited.exitCode, 9);

    serving = false;
    server.join();
    service.shutdown();
}

TEST_F(SessionServiceTest, RuntimeDirectoryAllowsSocketTraversalWithoutListing) {
    SessionService service({tempDir.string()});
    const std::string socketPath = (tempDir / "sessiond.sock").string();
    ASSERT_TRUE(service.initialize(socketPath));

    struct stat status {};
    ASSERT_EQ(stat(tempDir.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0711);
}
