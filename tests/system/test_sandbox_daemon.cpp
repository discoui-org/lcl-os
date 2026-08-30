#include <gtest/gtest.h>

#include "system/security/sandbox_daemon.hpp"

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

TEST(SandboxDaemonTest, AcceptsOnlyPreviouslyVerifiedApplicationRecords) {
    SandboxDaemon daemon({.socketPath = "/Runtime/lcl-sandboxd-test.sock",
                          .sessionUid = 61002,
                          .sessionGid = 61002});
    const VerifiedApplication application = validApplication();
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(application, {"network.client"}, 101, error);
    ASSERT_TRUE(plan.has_value()) << error;

    SandboxLaunchResult result = daemon.handleLaunchRequest(plan->request);
    EXPECT_EQ(result.status, SandboxLaunchStatus::PolicyRejected);
    EXPECT_NE(result.message.find("no registered verified application"), std::string::npos);

    ASSERT_TRUE(daemon.registerVerifiedApplication(application, {"network.client"}, error)) << error;
    EXPECT_EQ(daemon.registeredApplicationCount(), 1U);
    result = daemon.handleLaunchRequest(plan->request);
    EXPECT_EQ(result.status, SandboxLaunchStatus::SetupFailed);
    EXPECT_EQ(result.instanceId, 101U);
    EXPECT_NE(result.message.find("kernel enforcement"), std::string::npos);
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

TEST(SandboxProtocolTest, RoundTripsBoundedErrorDiagnostics) {
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSandboxError("sandbox request is invalid", payload));
    std::string decoded;
    EXPECT_TRUE(decodeSandboxError(payload, decoded));
    EXPECT_EQ(decoded, "sandbox request is invalid");
    EXPECT_FALSE(encodeSandboxError("", payload));
}

} // namespace
} // namespace lcl::security
