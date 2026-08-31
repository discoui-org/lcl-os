#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "system/security/sandbox_contract.hpp"
#include "system/security/sandbox_cgroup.hpp"
#include "system/security/sandbox_launch_authorizer.hpp"
#include "system/security/sandbox_protocol.hpp"
#include "system/security/sha256.hpp"

using namespace lcl::security;

namespace {

namespace fs = std::filesystem;

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
    EXPECT_EQ(plan->profile.profileId, "lcl.app-capability.native.v1");
    EXPECT_TRUE(plan->profile.noNewPrivileges);
    EXPECT_TRUE(plan->profile.requireSeccomp);
    EXPECT_TRUE(plan->profile.privateFilesystemView);
    EXPECT_TRUE(plan->profile.isolatedNetworkView);
    EXPECT_TRUE(plan->profile.denyDirectDeviceAccess);
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

TEST(SandboxContractTest, BuildsAnIdentityFreeProfileForTheSessionLaunchRequest) {
    std::string error;
    const auto profile = makeThirdPartySandboxProfile(
        SandboxRuntime::JavaScript, {"files.user-selected", "network.client"},
        {"network.client"}, error);
    ASSERT_TRUE(profile.has_value()) << error;
    EXPECT_EQ(profile->profileId, "lcl.app-capability.javascript.v1");
    EXPECT_TRUE(profile->isolatedNetworkView);
    EXPECT_EQ(profile->effectivePermissions, std::vector<std::string>({"network.client"}));

    EXPECT_FALSE(makeThirdPartySandboxProfile(static_cast<SandboxRuntime>(99), {}, {}, error));
    EXPECT_EQ(error, "sandbox profile has an unsupported runtime");
}

TEST(SandboxContractTest, RejectsMalformedRequestAndWeakenedProfile) {
    std::string error;
    SandboxLaunchRequest request{};
    request.appId = "org.example.notes";
    request.instanceId = 1;
    request.bundleRecordDigest = sha256("bundle");
    EXPECT_FALSE(validateSandboxLaunchRequest(request, error));

    SandboxProfile profile{};
    profile.profileId = "lcl.app-capability.native.v1";
    profile.privateFilesystemView = false;
    EXPECT_FALSE(validateSandboxProfile(profile, error));
    EXPECT_EQ(error, "common app capability profile weakens a mandatory security boundary");

    profile.privateFilesystemView = true;
    profile.isolatedNetworkView = false;
    EXPECT_FALSE(validateSandboxProfile(profile, error));
    EXPECT_EQ(error, "common app capability profile weakens a mandatory security boundary");

    SandboxPlatformHardening hardening{};
    hardening.resourceLimits.cpuWeight = 10001;
    EXPECT_FALSE(validateSandboxPlatformHardening(hardening, error));
    EXPECT_EQ(error, "sandbox platform hardening weakens a mandatory enforcement boundary");
}

TEST(SandboxCgroupTest, RefusesToCreateAnInstanceBeforeRootSetup) {
    std::string error;
    const auto plan = makeThirdPartySandboxLaunchPlan(validApplication(), {}, 81, error);
    ASSERT_TRUE(plan.has_value()) << error;

    SandboxCgroupManager manager;
    EXPECT_FALSE(manager.createInstance(*plan, SandboxPlatformHardening{}, error).has_value());
    EXPECT_EQ(error, "sandbox cgroup received an unverified launch plan");
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

TEST(SandboxProtocolTest, RoundTripsOnlyImmutableExternalRegistrationFields) {
    SandboxApplicationRegistration registration{};
    registration.userUid = 1000;
    registration.appId = "org.example.external";
    registration.runtime = SandboxRuntime::Native;
    registration.requestedPermissions = {"network.client"};
    registration.bundleRecordDigest = sha256("approved external bundle");
    registration.publisherIdentity = "unverified";
    registration.executableBundlePath = "Executables/app";

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(encodeSandboxApplicationRegistration(registration, payload));
    SandboxHeader header{};
    header.opcode = SandboxOpcode::RegisterApplication;
    header.requestId = 91;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    ASSERT_TRUE(encodeSandboxPacket(header, payload, packet));

    DecodedSandboxPacket decoded{};
    ASSERT_TRUE(decodeSandboxPacket(packet.data(), packet.size(), decoded));
    SandboxApplicationRegistration received{};
    ASSERT_TRUE(decodeSandboxApplicationRegistration(decoded.payload, received));
    EXPECT_EQ(received.userUid, 1000u);
    EXPECT_EQ(received.appId, registration.appId);
    EXPECT_EQ(received.bundleRecordDigest, registration.bundleRecordDigest);
    EXPECT_EQ(received.publisherIdentity, "unverified");
    EXPECT_EQ(received.executableBundlePath, "Executables/app");
    EXPECT_EQ(received.requestedPermissions,
              std::vector<std::string>({"network.client"}));
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

TEST(SandboxLaunchAuthorizerTest,
     RechecksLivePermissionDecisionAndRejectsAStaleWidenedProfile) {
    const fs::path directory = fs::temp_directory_path() /
        ("lcl-authorizer-permissions-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(fs::create_directories(directory));
    const PermissionStoreConfig config{
        .storePath = (directory / "permissions.v1").string(),
        .ownerUid = getuid(),
        .ownerGid = getgid(),
    };
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{directory};

    VerifiedApplication application = validApplication();
    application.permissionSubject = makeSystemImagePermissionSubject(
        1000, application.appId, application.bundleRecordDigest);
    PermissionStore writer(config);
    std::string error;
    ASSERT_TRUE(writer.setDecision(*application.permissionSubject, "network.client", true, error))
        << error;

    SandboxLaunchAuthorizer authorizer(config);
    ASSERT_TRUE(authorizer.registerVerifiedApplication(application, {}, error)) << error;
    const auto networkedPlan = makeThirdPartySandboxLaunchPlan(
        application, {"network.client"}, 81, error);
    ASSERT_TRUE(networkedPlan.has_value()) << error;
    ASSERT_TRUE(authorizer.authorize(networkedPlan->request, error).has_value()) << error;

    ASSERT_TRUE(writer.revoke(*application.permissionSubject, "network.client", error)) << error;
    EXPECT_FALSE(authorizer.authorize(networkedPlan->request, error).has_value());
    EXPECT_EQ(error, "sandbox launch request profile digest does not match the verified policy");

    const auto defaultDenyPlan = makeThirdPartySandboxLaunchPlan(application, {}, 81, error);
    ASSERT_TRUE(defaultDenyPlan.has_value()) << error;
    ASSERT_TRUE(authorizer.authorize(defaultDenyPlan->request, error).has_value()) << error;
}
