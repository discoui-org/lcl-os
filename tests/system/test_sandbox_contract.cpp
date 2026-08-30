#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "system/security/sandbox_contract.hpp"
#include "system/security/sandbox_launch_authorizer.hpp"
#include "system/security/sandbox_protocol.hpp"
#include "system/security/sha256.hpp"

using namespace lcl::security;

namespace {

VerifiedApplication validApplication() {
    VerifiedApplication application{};
    application.appId = "org.example.notes";
    application.identity = {application.appId, 61042, 61042};
    application.runtime = SandboxRuntime::Native;
    application.requestedPermissions = {"files.user-selected", "network.client"};
    application.bundleRecordDigest = sha256("org.example.notes bundle record");
    return application;
}

} // namespace

TEST(Sha256Test, ProducesKnownDigest) {
    EXPECT_EQ(hexEncodeDigest(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SandboxContractTest, BuildsDefaultDenyPlanFromVerifiedInputs) {
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(
        validApplication(), {"network.client", "files.user-selected"}, 41, error);

    ASSERT_TRUE(plan.has_value()) << error;
    EXPECT_EQ(plan->profile.profileId, "lcl.third-party.native.v1");
    EXPECT_TRUE(plan->profile.privateMountNamespace);
    EXPECT_TRUE(plan->profile.privatePidNamespace);
    EXPECT_TRUE(plan->profile.privateIpcNamespace);
    EXPECT_TRUE(plan->profile.privateNetworkNamespace);
    EXPECT_TRUE(plan->profile.noNewPrivileges);
    EXPECT_TRUE(plan->profile.requireSeccomp);
    EXPECT_TRUE(plan->profile.requireLandlock);
    EXPECT_TRUE(plan->profile.denyDirectDeviceAccess);
    EXPECT_TRUE(plan->profile.allowNetworkClient);
    EXPECT_EQ(plan->profile.memoryMaxMiB, 512u);
    EXPECT_EQ(plan->profile.pidsMax, 64u);
    EXPECT_EQ(plan->profile.effectivePermissions,
              std::vector<std::string>({"files.user-selected", "network.client"}));
    EXPECT_EQ(plan->request.appId, "org.example.notes");
    EXPECT_EQ(plan->request.instanceId, 41u);
    EXPECT_FALSE(isZeroDigest(plan->request.bundleRecordDigest));
    EXPECT_FALSE(isZeroDigest(plan->request.profileDigest));
    EXPECT_TRUE(validateSandboxLaunchRequest(plan->request, error)) << error;
}

TEST(SandboxContractTest, RejectsAGrantMissingFromManifest) {
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(
        validApplication(), {"network.client", "camera"}, 41, error);

    EXPECT_FALSE(plan.has_value());
    EXPECT_EQ(error, "sandbox policy tried to grant a permission absent from the manifest");
}

TEST(SandboxContractTest, CanonicalizesPermissionsBeforeDigestingProfile) {
    std::string error;
    const auto first = makeThirdPartySandboxLaunchPlan(
        validApplication(), {"network.client", "files.user-selected"}, 41, error);
    ASSERT_TRUE(first.has_value()) << error;
    const auto second = makeThirdPartySandboxLaunchPlan(
        validApplication(), {"files.user-selected", "network.client"}, 41, error);
    ASSERT_TRUE(second.has_value()) << error;

    EXPECT_EQ(first->request.profileDigest, second->request.profileDigest);
}

TEST(SandboxContractTest, RejectsMalformedRequestAndWeakenedProfile) {
    std::string error;
    SandboxLaunchRequest request{};
    request.appId = "org.example.notes";
    request.instanceId = 1;
    request.bundleRecordDigest = sha256("bundle");
    EXPECT_FALSE(validateSandboxLaunchRequest(request, error));

    SandboxProfile profile{};
    profile.profileId = "lcl.third-party.native.v1";
    profile.privatePidNamespace = false;
    EXPECT_FALSE(validateSandboxProfile(profile, error));
    EXPECT_EQ(error, "third-party sandbox profile weakens a mandatory security boundary");
}

TEST(SandboxProtocolTest, RoundTripsOnlyTheBoundLaunchFields) {
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(
        validApplication(), {"network.client"}, 991, error);
    ASSERT_TRUE(plan.has_value()) << error;

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSandboxLaunchRequest(plan->request, payload));
    SandboxHeader header{};
    header.opcode = SandboxOpcode::LaunchRequest;
    header.requestId = 77;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(encodeSandboxPacket(header, payload, packet));

    DecodedSandboxPacket decoded{};
    ASSERT_TRUE(decodeSandboxPacket(packet.data(), packet.size(), decoded));
    SandboxLaunchRequest request{};
    ASSERT_TRUE(decodeSandboxLaunchRequest(decoded.payload, request));
    EXPECT_EQ(request.appId, "org.example.notes");
    EXPECT_EQ(request.instanceId, 991u);
    EXPECT_EQ(request.bundleRecordDigest, plan->request.bundleRecordDigest);
    EXPECT_EQ(request.profileDigest, plan->request.profileDigest);
}

TEST(SandboxProtocolTest, RejectsTruncatedOrMalformedLaunchData) {
    std::vector<std::uint8_t> malformed(12, 0);
    SandboxLaunchRequest request{};
    EXPECT_FALSE(decodeSandboxLaunchRequest(malformed, request));

    SandboxLaunchResult launched{};
    launched.status = SandboxLaunchStatus::Launched;
    launched.instanceId = 1;
    launched.pid = 0;
    launched.processGroupId = 1;
    std::vector<std::uint8_t> payload;
    EXPECT_FALSE(encodeSandboxLaunchResult(launched, payload));
}

TEST(SandboxLaunchAuthorizerTest, RebuildsThePlanFromItsProtectedRecord) {
    SandboxLaunchAuthorizer authorizer;
    std::string error;
    const VerifiedApplication application = validApplication();
    ASSERT_TRUE(authorizer.registerVerifiedApplication(application, {"network.client"}, error)) << error;

    const auto expected = makeThirdPartySandboxLaunchPlan(application, {"network.client"}, 77, error);
    ASSERT_TRUE(expected.has_value()) << error;
    const auto authorized = authorizer.authorize(expected->request, error);

    ASSERT_TRUE(authorized.has_value()) << error;
    EXPECT_EQ(authorized->request.profileDigest, expected->request.profileDigest);
    EXPECT_EQ(authorized->profile.effectivePermissions,
              std::vector<std::string>({"network.client"}));
}

TEST(SandboxLaunchAuthorizerTest, RejectsUnregisteredAndMismatchedRequests) {
    SandboxLaunchAuthorizer authorizer;
    std::string error;
    const VerifiedApplication application = validApplication();
    const auto plan = makeThirdPartySandboxLaunchPlan(application, {"network.client"}, 77, error);
    ASSERT_TRUE(plan.has_value()) << error;

    EXPECT_FALSE(authorizer.authorize(plan->request, error).has_value());
    EXPECT_EQ(error, "sandbox launch request has no registered verified application");

    ASSERT_TRUE(authorizer.registerVerifiedApplication(application, {"network.client"}, error)) << error;
    SandboxLaunchRequest wrongBundle = plan->request;
    wrongBundle.bundleRecordDigest = sha256("different verified bundle");
    EXPECT_FALSE(authorizer.authorize(wrongBundle, error).has_value());
    EXPECT_EQ(error, "sandbox launch request bundle digest does not match the verified record");

    SandboxLaunchRequest wrongProfile = plan->request;
    wrongProfile.profileDigest = sha256("different profile");
    EXPECT_FALSE(authorizer.authorize(wrongProfile, error).has_value());
    EXPECT_EQ(error, "sandbox launch request profile digest does not match the verified policy");
}
