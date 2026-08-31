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

TEST_F(SessionServiceTest, ServiceRejectsThirdPartyDirectLaunchUntilSandboxdEnforcesIt) {
    createBundle("Lifecycle.app", "org.lcl.lifecycle");
    SessionService service({tempDir.string()});
    service.refreshCatalog();
    const LaunchResponse launch = service.launch({"org.lcl.lifecycle", false});
    EXPECT_EQ(launch.status, 3u);
    EXPECT_EQ(launch.appId, "org.lcl.lifecycle");
    EXPECT_EQ(launch.instanceId, 0u);
    EXPECT_EQ(launch.pid, 0);
    EXPECT_NE(launch.message.find("lcl-sandboxd"), std::string::npos);
    EXPECT_TRUE(service.instances().empty());
}

TEST_F(SessionServiceTest, ClientReceivesThirdPartyLaunchRefusalFromSessiond) {
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
    const bool received = client.launch({"org.lcl.rpc", true}, response, error);
    EXPECT_TRUE(received) << error;
    if (received) {
        EXPECT_EQ(response.status, 3u);
        EXPECT_EQ(response.appId, "org.lcl.rpc");
        EXPECT_EQ(response.instanceId, 0u);
        EXPECT_EQ(response.pid, 0);
        EXPECT_NE(response.message.find("lcl-sandboxd"), std::string::npos);
    }

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
