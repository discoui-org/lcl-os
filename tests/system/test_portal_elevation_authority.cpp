#include <gtest/gtest.h>

#include "system/security/app_identity_registry.hpp"
#include "system/security/app_launch_registry.hpp"
#include "system/security/elevation_authority.hpp"
#include "system/security/portal_authority.hpp"

#include <chrono>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

class PortalElevationAuthorityTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = fs::temp_directory_path() /
            ("lcl-portal-elevation-" + std::to_string(getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory_);
        ASSERT_EQ(chmod(directory_.c_str(), 0700), 0);
        identities_ = {.registryPath = (directory_ / "identities.v1").string(),
                       .firstAppUid = 61000, .lastAppUid = 61010,
                       .ownerUid = getuid(), .ownerGid = getgid()};
        launches_ = {.directoryPath = (directory_ / "launches").string(),
                     .firstAppUid = 61000, .lastAppUid = 61010,
                     .ownerUid = getuid(), .ownerGid = getgid()};
        permissions_ = {.storePath = (directory_ / "permissions.v1").string(),
                        .ownerUid = getuid(), .ownerGid = getgid()};
        audit_ = {.path = (directory_ / "admin-audit.v1").string(),
                  .ownerUid = getuid(), .ownerGid = getgid()};

        AppIdentityRegistry identities(identities_);
        std::string error;
        identity_ = identities.getOrCreate("org.lcl.portal-test", error);
        ASSERT_TRUE(identity_) << error;
        AppLaunchRegistry launches(launches_);
        ASSERT_TRUE(launches.initializeAndReset(error)) << error;
        ASSERT_TRUE(launches.registerLaunch(
            {77, identity_->appId, getpgrp(), identity_->uid, identity_->gid}, error)) << error;
        subject_ = {.userUid = 1000, .appId = identity_->appId,
                    .bundleRecordDigest = sha256("portal-bundle"),
                    .publisherIdentity = "unverified"};
    }

    void TearDown() override { fs::remove_all(directory_); }

    ApplicationPeerAuthenticatorConfig peers() const {
        return {.identities = identities_, .launches = launches_,
                .sessionUid = 45000, .sessionGid = 45000};
    }

    fs::path directory_;
    AppIdentityRegistryConfig identities_;
    AppLaunchRegistryConfig launches_;
    PermissionStoreConfig permissions_;
    ElevationAuditStoreConfig audit_;
    std::optional<AppIdentity> identity_;
    PermissionSubject subject_;
};

TEST_F(PortalElevationAuthorityTest, BindsPromptAndDecisionToAuthenticatedLiveInstance) {
    PortalAuthority authority(peers(), permissions_);
    const PortalRequest request{.requestId = 9, .instanceId = 77,
                                .appId = identity_->appId,
                                .operation = PortalOperation::ReadClipboard,
                                .scope = "current clipboard contents"};
    const PortalPeerCredentials peer{getpid(), identity_->uid, identity_->gid};
    std::string error;
    EXPECT_EQ(authority.authorize(peer, request, subject_, {"clipboard.read"}, error),
              PortalAuthorization::NeedsVisibleConsent) << error;
    const auto prompt = authority.makeTrustedPrompt(request, subject_, "Portal Test", error);
    ASSERT_TRUE(prompt) << error;
    EXPECT_EQ(prompt->publisherIdentity, "unverified");
    EXPECT_EQ(prompt->allowedDurations.size(), 3u);
    ASSERT_TRUE(authority.recordVisibleDecision(request, subject_, true,
                                                PortalConsentDuration::Persistent, error)) << error;
    EXPECT_EQ(authority.authorize(peer, PortalRequest{.requestId = 10, .instanceId = 77,
                  .appId = identity_->appId, .operation = PortalOperation::ReadClipboard,
                  .scope = "current clipboard contents"}, subject_, {"clipboard.read"}, error),
              PortalAuthorization::Granted) << error;
}

TEST_F(PortalElevationAuthorityTest, IssuesOneShotElevationOnlyAfterTrustedInteraction) {
    PermissionStore permissions(permissions_);
    std::string error;
    ASSERT_TRUE(permissions.setDecision(subject_, "admin.elevation", true, error)) << error;
    ElevationAuthority authority(peers(), permissions_, audit_);
    const ElevationRequest request{.requestId = 31, .instanceId = 77,
        .appId = identity_->appId, .bundleRecordDigest = subject_.bundleRecordDigest,
        .publisherIdentity = subject_.publisherIdentity,
        .reason = "Change one protected setting",
        .scope = ElevationScope::WriteSystemSetting,
        .target = "appearance.theme"};
    const ElevationPeerCredentials peer{getpid(), identity_->uid, identity_->gid};
    const TrustedInteractionContext interaction{identity_->appId, 77, 101, true, true};
    const auto prompt = authority.request(peer, request, subject_, {"admin.elevation"},
                                          interaction, "Portal Test", error);
    ASSERT_TRUE(prompt) << error;
    const auto grant = authority.decide(31, true, std::chrono::seconds(30), error);
    ASSERT_TRUE(grant) << error;
    EXPECT_TRUE(authority.consume(*grant, error)) << error;
    EXPECT_FALSE(authority.consume(*grant, error));
    struct stat auditStatus {};
    ASSERT_EQ(stat(audit_.path.c_str(), &auditStatus), 0);
    EXPECT_EQ(auditStatus.st_mode & 0777, 0600);
}

} // namespace
} // namespace lcl::security
