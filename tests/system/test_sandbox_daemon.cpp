#include <gtest/gtest.h>

#include "system/security/sandbox_daemon.hpp"

#include <sys/wait.h>
#include <unistd.h>

namespace lcl::security {
namespace {

VerifiedApplication validApplication() {
    VerifiedApplication application{};
    application.appId = "org.lcl.sandbox-daemon";
    application.identity = {application.appId, 61001, 61001};
    application.runtime = SandboxRuntime::Native;
    application.requestedPermissions = {"network.client"};
    application.bundleRecordDigest = sha256("sandboxd-record");
    return application;
}

TEST(SandboxDaemonTest, RefusesLaunchBeforePlatformHardeningEvenForRegisteredApplications) {
    SandboxDaemon daemon({.socketPath = "/Runtime/lcl-sandboxd-test.sock",
                          .sessionUid = 61002,
                          .sessionGid = 61002});
    const VerifiedApplication application = validApplication();
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(application, {"network.client"}, 101, error);
    ASSERT_TRUE(plan.has_value()) << error;

    SandboxLaunchResult result = daemon.handleLaunchRequest(plan->request);
    EXPECT_EQ(result.status, SandboxLaunchStatus::SetupFailed);
    EXPECT_NE(result.message.find("platform hardening is not initialized"), std::string::npos);
    EXPECT_EQ(result.pid, 0);
    EXPECT_EQ(daemon.runningChildCount(), 0U);

    ASSERT_TRUE(daemon.registerVerifiedApplication(application, {"network.client"}, error)) << error;
    EXPECT_EQ(daemon.registeredApplicationCount(), 1U);
    result = daemon.handleLaunchRequest(plan->request);
    EXPECT_EQ(result.status, SandboxLaunchStatus::SetupFailed);
    EXPECT_EQ(result.instanceId, 101U);
    EXPECT_NE(result.message.find("platform hardening is not initialized"), std::string::npos);
    EXPECT_EQ(result.pid, 0);
    EXPECT_EQ(daemon.runningChildCount(), 0U);
}

TEST(SandboxDaemonTest, DoesNotTurnMalformedRequestsIntoLaunches) {
    SandboxDaemon daemon({.socketPath = "/Runtime/lcl-sandboxd-test.sock",
                          .sessionUid = 61002,
                          .sessionGid = 61002});
    SandboxLaunchRequest request{};
    request.instanceId = 12;
    const SandboxLaunchResult result = daemon.handleLaunchRequest(request);
    EXPECT_EQ(result.status, SandboxLaunchStatus::InvalidRequest);
    EXPECT_EQ(result.instanceId, 12U);
}

TEST(SandboxChildReaperTest, ReportsOnlyTheDaemonOwnedChildExit) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        _exit(23);
    }

    SandboxChildReaper reaper;
    SandboxLaunchResult launch{};
    launch.status = SandboxLaunchStatus::Launched;
    launch.instanceId = 103;
    launch.pid = static_cast<std::int32_t>(child);
    launch.processGroupId = static_cast<std::int32_t>(child);
    const SandboxCgroup cgroup{.instanceId = 103,
                                .path = "/sys/fs/cgroup/lcl/apps/instance-103"};
    std::string error;
    ASSERT_TRUE(reaper.track("org.lcl.sandbox-daemon", launch, cgroup, error)) << error;

    std::vector<SandboxChildExit> exited;
    for (int attempt = 0; attempt < 100 && exited.empty(); ++attempt) {
        exited = reaper.reap();
        if (exited.empty()) {
            usleep(1000);
        }
    }
    ASSERT_EQ(exited.size(), 1U);
    EXPECT_EQ(exited.front().instanceId, 103U);
    EXPECT_EQ(exited.front().exitCode, 23);
    ASSERT_TRUE(exited.front().cgroup.has_value());
    EXPECT_EQ(exited.front().cgroup->path, cgroup.path);
    EXPECT_EQ(reaper.size(), 0U);
}

TEST(SandboxProtocolTest, RoundTripsBoundedErrorDiagnostics) {
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSandboxError("sandbox request is invalid", payload));
    std::string decoded;
    EXPECT_TRUE(decodeSandboxError(payload, decoded));
    EXPECT_EQ(decoded, "sandbox request is invalid");
    EXPECT_FALSE(encodeSandboxError("", payload));
}

TEST(SandboxProtocolTest, RoundTripsSandboxProcessExit) {
    SandboxProcessExited expected{};
    expected.instanceId = 17;
    expected.exitCode = 137;
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSandboxProcessExited(expected, payload));
    SandboxProcessExited decoded{};
    ASSERT_TRUE(decodeSandboxProcessExited(payload, decoded));
    EXPECT_EQ(decoded.instanceId, expected.instanceId);
    EXPECT_EQ(decoded.exitCode, expected.exitCode);
}

} // namespace
} // namespace lcl::security
