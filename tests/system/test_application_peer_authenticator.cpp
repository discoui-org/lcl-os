#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "system/security/application_peer_authenticator.hpp"

namespace fs = std::filesystem;
using namespace lcl::security;

class ApplicationPeerAuthenticatorTest : public ::testing::Test {
protected:
    fs::path tempDir;
    AppIdentityRegistryConfig identities;
    AppLaunchRegistryConfig launches;

    void SetUp() override {
        tempDir = fs::temp_directory_path() /
            ("lcl-peer-authenticator-" + std::to_string(getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
        fs::create_directories(tempDir);
        chmod(tempDir.c_str(), 0700);
        identities = {
            .registryPath = (tempDir / "identities.v1").string(),
            .firstAppUid = 61000,
            .lastAppUid = 61009,
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        };
        launches = {
            .directoryPath = (tempDir / "launches").string(),
            .firstAppUid = 61000,
            .lastAppUid = 61009,
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        };
        AppLaunchRegistry registry(launches);
        std::string error;
        ASSERT_TRUE(registry.initializeAndReset(error)) << error;
    }

    void TearDown() override { fs::remove_all(tempDir); }

    ApplicationPeerAuthenticator makeAuthenticator() const {
        return ApplicationPeerAuthenticator({
            .identities = identities,
            .launches = launches,
            .sessionUid = 45000,
            .sessionGid = 45000,
        });
    }
};

TEST_F(ApplicationPeerAuthenticatorTest, AcceptsExactRegistryIdentity) {
    AppIdentityRegistry registry(identities);
    std::string error;
    const auto identity = registry.getOrCreate("org.lcl.sandbox-test", error);
    ASSERT_TRUE(identity) << error;
    AppLaunchRegistry launchRegistry(launches);
    ASSERT_TRUE(launchRegistry.registerLaunch(
        {41, identity->appId, getpgrp(), identity->uid, identity->gid}, error))
        << error;

    const auto authenticator = makeAuthenticator();
    EXPECT_TRUE(authenticator.authenticate(
        identity->appId, 41, getpid(), identity->uid, identity->gid, error))
        << error;
    EXPECT_FALSE(authenticator.authenticate(
        identity->appId, 42, getpid(), identity->uid, identity->gid, error));

    ASSERT_TRUE(launchRegistry.registerLaunch(
        {42, identity->appId, getpgrp() + 1, identity->uid, identity->gid}, error))
        << error;
    EXPECT_FALSE(authenticator.authenticate(
        identity->appId, 42, getpid(), identity->uid, identity->gid, error));
    EXPECT_EQ(error, "peer is not a member of the claimed app launch instance");
}

TEST_F(ApplicationPeerAuthenticatorTest, RejectsClaimForAnotherSandboxUid) {
    AppIdentityRegistry registry(identities);
    std::string error;
    const auto first = registry.getOrCreate("org.lcl.first", error);
    ASSERT_TRUE(first) << error;
    const auto second = registry.getOrCreate("org.lcl.second", error);
    ASSERT_TRUE(second) << error;

    const auto authenticator = makeAuthenticator();
    EXPECT_FALSE(authenticator.authenticate(
        second->appId, 1, getpid(), first->uid, first->gid, error));
    EXPECT_EQ(error, "peer credentials do not match the claimed app ID");
}

TEST_F(ApplicationPeerAuthenticatorTest, RejectsRootAndSplitCredentials) {
    const auto authenticator = makeAuthenticator();
    std::string error;

    EXPECT_FALSE(authenticator.authenticate(
        "org.lcl.sandbox-test", 1, getpid(), 0, 0, error));
    EXPECT_FALSE(authenticator.authenticate(
        "org.lcl.sandbox-test", 1, getpid(), 61000, 61001, error));
}

TEST_F(ApplicationPeerAuthenticatorTest, SessionUidCanOnlyClaimTrustedSystemIds) {
    const auto authenticator = makeAuthenticator();
    std::string error;

    EXPECT_TRUE(authenticator.authenticate(
        "org.lcl.desktop-shell", 0, getpid(), 45000, 45000, error)) << error;
    EXPECT_TRUE(authenticator.authenticate(
        kTrustedUserShellAppId, 7, getpid(), 45000, 45000, error)) << error;
    EXPECT_TRUE(authenticator.authenticate(
        kSystemSettingsAppId, 8, getpid(), 45000, 45000, error)) << error;
    EXPECT_FALSE(authenticator.authenticate(
        "org.lcl.sandbox-test", 1, getpid(), 45000, 45000, error));
    EXPECT_FALSE(authenticator.authenticate(
        "org.lcl.desktop-shell", 1, getpid(), 45000, 45001, error));
}

TEST_F(ApplicationPeerAuthenticatorTest, FailsClosedForUnsafeRegistry) {
    AppIdentityRegistry registry(identities);
    std::string error;
    const auto identity = registry.getOrCreate("org.lcl.sandbox-test", error);
    ASSERT_TRUE(identity) << error;
    ASSERT_EQ(chmod(identities.registryPath.c_str(), 0644), 0);

    const auto authenticator = makeAuthenticator();
    EXPECT_FALSE(authenticator.authenticate(
        identity->appId, 1, getpid(), identity->uid, identity->gid, error));
    EXPECT_FALSE(error.empty());
}
