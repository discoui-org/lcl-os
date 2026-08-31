#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "system/security/bundle_record.hpp"
#include "system/security/bundle_approval_store.hpp"
#include "system/security/sandbox_contract.hpp"
#include "system/security/sandbox_protocol.hpp"
#include "system/session/app_bundle_parser.hpp"
#include "system/session/app_registry.hpp"
#include "system/session/session_client.hpp"
#include "system/session/session_protocol.hpp"
#include "system/session/session_service.hpp"

namespace fs = std::filesystem;
using namespace lcl::session;

namespace {

bool waitForFlag(const std::atomic<bool> &flag) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (flag.load()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool sendSandboxPacket(int descriptor, lcl::security::SandboxOpcode opcode,
                       std::uint32_t requestId,
                       const std::vector<std::uint8_t> &payload) {
  lcl::security::SandboxHeader header{};
  header.opcode = opcode;
  header.requestId = requestId;
  header.payloadSize = static_cast<std::uint32_t>(payload.size());
  std::vector<std::uint8_t> packet;
  return lcl::security::encodeSandboxPacket(header, payload, packet) &&
         send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(packet.size());
}

bool receiveSandboxPacketWithDescriptors(
    int descriptor, lcl::security::DecodedSandboxPacket &packet,
    std::array<int, 2> &receivedDescriptors) {
  std::array<std::uint8_t, lcl::security::kSandboxWireHeaderSize +
                               lcl::security::kSandboxMaxPayload>
      bytes{};
  std::array<char, CMSG_SPACE(sizeof(int) * 2)> control{};
  iovec vector{.iov_base = bytes.data(), .iov_len = bytes.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  const ssize_t count = recvmsg(descriptor, &message, 0);
  cmsghdr *descriptorMessage = CMSG_FIRSTHDR(&message);
  if (count <= 0 || (message.msg_flags & MSG_CTRUNC) != 0 ||
      !descriptorMessage || descriptorMessage->cmsg_level != SOL_SOCKET ||
      descriptorMessage->cmsg_type != SCM_RIGHTS ||
      descriptorMessage->cmsg_len != CMSG_LEN(sizeof(int) * 2) ||
      CMSG_NXTHDR(&message, descriptorMessage) != nullptr ||
      !lcl::security::decodeSandboxPacket(
          bytes.data(), static_cast<std::size_t>(count), packet)) {
    return false;
  }
  std::memcpy(receivedDescriptors.data(), CMSG_DATA(descriptorMessage),
              sizeof(int) * receivedDescriptors.size());
  return true;
}

} // namespace

class SessionServiceTest : public ::testing::Test {
protected:
  fs::path tempDir;

  void SetUp() override {
    tempDir =
        fs::temp_directory_path() /
        ("lcl-session-test-" + std::to_string(::getpid()) + "-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tempDir);
    chmod(tempDir.c_str(), 0700);
  }
  void TearDown() override { fs::remove_all(tempDir); }

  fs::path
  createBundle(const std::string &directory, const std::string &appId,
               const std::string &executableBody = "#!/bin/sh\nexit 23\n") {
    const fs::path bundle = tempDir / directory;
    fs::create_directories(bundle / "Executables");
    fs::create_directories(bundle / "Resources");
    std::ofstream manifest(bundle / "Manifest.json");
    manifest << "{\"id\":\"" << appId
             << "\",\"name\":\"Test App\",\"version\":\"1.0\","
                "\"executable\":\"Executables/test-app\",\"icon\":\"Resources/"
                "Icon.png\",\"runtime\":\"org.lcl.native\",\"type\":\"gui\"}";
    manifest.close();
    std::ofstream icon(bundle / "Resources" / "Icon.png");
    icon << "icon";
    icon.close();
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
  const fs::path first =
      createBundle("HigherPriority/First.app", "org.lcl.duplicate");
  createBundle("LowerPriority/Second.app", "org.lcl.duplicate");

  AppRegistry registry({(tempDir / "HigherPriority").string(),
                        (tempDir / "LowerPriority").string()});
  registry.refresh();

  ASSERT_EQ(registry.entries().size(), 1u);
  EXPECT_EQ(registry.entries().front().appId, "org.lcl.duplicate");
  EXPECT_EQ(registry.entries().front().bundlePath, first.string());
}

TEST_F(SessionServiceTest,
       ServiceRejectsThirdPartyDirectLaunchUntilSandboxdEnforcesIt) {
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

TEST_F(SessionServiceTest,
       DelegatesThirdPartyLaunchAndExitLifecycleToSandboxd) {
  const fs::path bundle = createBundle("Sandboxed.app", "org.lcl.sandboxed");
  const auto metadata =
      lcl::core::AppBundleParser::parseBundle(bundle.string());
  ASSERT_TRUE(metadata.has_value());
  std::string recordError;
  const auto record = lcl::security::makeBundleRecord(*metadata, recordError);
  ASSERT_TRUE(record.has_value()) << recordError;
  std::string profileError;
  const auto profile = lcl::security::makeThirdPartySandboxProfile(
      lcl::security::SandboxRuntime::Native, metadata->requestedPermissions, {},
      profileError);
  ASSERT_TRUE(profile.has_value()) << profileError;

  const std::string sandboxSocketPath = (tempDir / "sandboxd.sock").string();
  const int listener =
      socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(sandboxSocketPath.size(), sizeof(address.sun_path));
  std::strncpy(address.sun_path, sandboxSocketPath.c_str(),
               sizeof(address.sun_path) - 1);
  ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)),
            0);
  ASSERT_EQ(listen(listener, 1), 0);

  std::atomic<bool> stop{false};
  std::atomic<bool> receivedRequest{false};
  std::atomic<bool> requestExit{false};
  std::atomic<bool> exitSent{false};
  std::string daemonError;
  const auto expectedBundleDigest = record->digest;
  const auto expectedProfileDigest =
      lcl::security::digestSandboxProfile(*profile);
  const lcl::security::PermissionStoreConfig permissionStoreConfig{
      .storePath = (tempDir / "permissions.v1").string(),
      .ownerUid = getuid(),
      .ownerGid = getgid(),
  };
  const lcl::security::BundleApprovalStoreConfig approvalStoreConfig{
      .storePath = (tempDir / "bundle-approvals.v1").string(),
      .ownerUid = getuid(),
      .ownerGid = getgid(),
  };
  const lcl::security::BundleSnapshotStoreConfig snapshotStoreConfig{
      .snapshotsRoot = (tempDir / "bundle-snapshots").string(),
      .ownerUid = getuid(),
      .ownerGid = getgid(),
  };
  lcl::security::BundleApprovalStore approvals(approvalStoreConfig);
  ASSERT_TRUE(approvals.approve(1000, *record,
                                lcl::security::BundlePublisherState::Unverified,
                                recordError))
      << recordError;
  std::thread daemon([&] {
    int client = -1;
    while (!stop.load()) {
      pollfd listenerPoll{.fd = listener, .events = POLLIN, .revents = 0};
      if (poll(&listenerPoll, 1, 10) <= 0) {
        continue;
      }
      client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0) {
        break;
      }
      if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        daemonError = std::string("accept failed: ") + std::strerror(errno);
        return;
      }
    }
    if (client < 0) {
      return;
    }

    lcl::security::DecodedSandboxPacket packet{};
    std::array<int, 2> receivedDescriptors{-1, -1};
    if (!receiveSandboxPacketWithDescriptors(client, packet,
                                             receivedDescriptors)) {
      daemonError = "sessiond did not send the immutable bundle snapshot descriptors";
      close(client);
      return;
    }
    lcl::security::SandboxApplicationRegistration registration{};
    struct stat bundleStatus{};
    struct stat executableStatus{};
    const bool validRegistration =
        packet.header.opcode == lcl::security::SandboxOpcode::RegisterApplication &&
        lcl::security::decodeSandboxApplicationRegistration(packet.payload, registration) &&
        registration.appId == "org.lcl.sandboxed" &&
        registration.bundleRecordDigest == expectedBundleDigest &&
        registration.executableBundlePath == "Executables/test-app" &&
        registration.publisherIdentity == "unverified" &&
        fstat(receivedDescriptors[0], &bundleStatus) == 0 &&
        S_ISDIR(bundleStatus.st_mode) && fstat(receivedDescriptors[1], &executableStatus) == 0 &&
        S_ISREG(executableStatus.st_mode);
    close(receivedDescriptors[0]);
    close(receivedDescriptors[1]);
    if (!validRegistration) {
      daemonError = "sessiond sent an invalid external bundle registration";
      close(client);
      return;
    }
    std::vector<std::uint8_t> payload;
    const lcl::security::SandboxRegistrationResult registered{
        .registered = true,
        .message = "registered",
    };
    if (!lcl::security::encodeSandboxRegistrationResult(registered, payload) ||
        !sendSandboxPacket(client,
                           lcl::security::SandboxOpcode::RegistrationResult,
                           packet.header.requestId, payload)) {
      daemonError = "could not acknowledge protected bundle registration";
      close(client);
      return;
    }

    std::array<std::uint8_t, lcl::security::kSandboxWireHeaderSize +
                                 lcl::security::kSandboxMaxPayload>
        packetBytes{};
    const ssize_t count = recv(client, packetBytes.data(), packetBytes.size(), 0);
    lcl::security::SandboxLaunchRequest request{};
    if (count <= 0 ||
        !lcl::security::decodeSandboxPacket(
            packetBytes.data(), static_cast<std::size_t>(count), packet) ||
        packet.header.opcode != lcl::security::SandboxOpcode::LaunchRequest ||
        !lcl::security::decodeSandboxLaunchRequest(packet.payload, request) ||
        request.appId != "org.lcl.sandboxed" || request.instanceId != 1 ||
        request.bundleRecordDigest != expectedBundleDigest ||
        request.profileDigest != expectedProfileDigest) {
      daemonError =
          "sessiond sent an invalid or widened sandbox launch request";
      close(client);
      return;
    }

    lcl::security::SandboxLaunchResult result{};
    result.status = lcl::security::SandboxLaunchStatus::Launched;
    result.instanceId = request.instanceId;
    // Linux PID values are bounded far below INT32_MAX, so teardown can
    // safely exercise lifecycle bookkeeping without signalling a process.
    result.pid = std::numeric_limits<std::int32_t>::max();
    result.processGroupId = result.pid;
    result.message = "launched";
    payload.clear();
    if (!lcl::security::encodeSandboxLaunchResult(result, payload) ||
        !sendSandboxPacket(client, lcl::security::SandboxOpcode::LaunchResult,
                           packet.header.requestId, payload)) {
      daemonError = "could not return the sandbox launch result";
      close(client);
      return;
    }
    receivedRequest = true;
    while (!stop.load() && !requestExit.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!stop.load()) {
      lcl::security::SandboxProcessExited exited{
          .instanceId = request.instanceId, .exitCode = 23};
      if (!lcl::security::encodeSandboxProcessExited(exited, payload) ||
          !sendSandboxPacket(client,
                             lcl::security::SandboxOpcode::ProcessExited, 1,
                             payload)) {
        daemonError = "could not return the sandbox exit event";
      } else {
        exitSent = true;
      }
    }
    // Keep the long-lived session-authority control channel open after a
    // normal app exit.  A real sandboxd does the same for later launches.
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    close(client);
  });

  SessionService service({}, sandboxSocketPath,
                         permissionStoreConfig, approvalStoreConfig,
                         snapshotStoreConfig);
  service.refreshCatalog();
  const LaunchResponse launch = service.launch({bundle.string(), false});
  EXPECT_EQ(launch.status, 0u) << launch.message;
  EXPECT_EQ(launch.appId, "org.lcl.sandboxed");
  EXPECT_EQ(launch.instanceId, 1u);
  EXPECT_EQ(launch.pid, std::numeric_limits<std::int32_t>::max());
  EXPECT_TRUE(waitForFlag(receivedRequest));

  if (launch.status == 0) {
    requestExit = true;
    EXPECT_TRUE(waitForFlag(exitSent));
    for (int attempt = 0; attempt < 50; ++attempt) {
      service.poll();
      const auto found = service.instances().find(launch.instanceId);
      if (found != service.instances().end() && !found->second.running) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto instance = service.instances().find(launch.instanceId);
    EXPECT_NE(instance, service.instances().end());
    if (instance != service.instances().end()) {
      EXPECT_TRUE(instance->second.sandboxed);
      EXPECT_FALSE(instance->second.running);
      EXPECT_EQ(instance->second.exitCode, 23);
    }
  }

  stop = true;
  service.shutdown();
  daemon.join();
  close(listener);
  unlink(sandboxSocketPath.c_str());
  EXPECT_TRUE(daemonError.empty()) << daemonError;
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

TEST_F(SessionServiceTest,
       RuntimeDirectoryAllowsSocketTraversalWithoutListing) {
  SessionService service({tempDir.string()});
  const std::string socketPath = (tempDir / "sessiond.sock").string();
  ASSERT_TRUE(service.initialize(socketPath));

  struct stat status{};
  ASSERT_EQ(stat(tempDir.c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & 0777, 0711);
}
