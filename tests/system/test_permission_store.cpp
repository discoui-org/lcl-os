#include <gtest/gtest.h>

#include "system/security/permission_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

class PermissionStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = fs::temp_directory_path() /
                     ("lcl-permissions-" + std::to_string(getpid()) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(directory_, error);
    }

    PermissionStoreConfig config() const {
        return {.storePath = (directory_ / "permissions.v1").string(),
                .ownerUid = getuid(), .ownerGid = getgid()};
    }

    static PermissionSubject subject(std::string bundleSeed = "bundle-a",
                                     uid_t userUid = 1000,
                                     std::string publisher = "unverified",
                                     std::uint32_t permissionVersion = kPermissionDecisionVersion) {
        return {.userUid = userUid,
                .appId = "org.lcl.permissions",
                .bundleRecordDigest = sha256(bundleSeed),
                .publisherIdentity = std::move(publisher),
                .permissionVersion = permissionVersion};
    }

    fs::path directory_;
};

TEST_F(PermissionStoreTest, DefaultsToDenyAndReturnsOnlyRequestedExactGrants) {
    PermissionStore store(config());
    const PermissionSubject app = subject();
    std::string error;
    ASSERT_TRUE(store.load(error)) << error;
    EXPECT_FALSE(store.isGranted(app, "network.client", error));
    EXPECT_TRUE(error.empty());

    ASSERT_TRUE(store.setDecision(app, "network.client", true, error)) << error;
    ASSERT_TRUE(store.setDecision(app, "files.documents.read", true, error)) << error;
    EXPECT_TRUE(store.isGranted(app, "network.client", error));

    const std::vector<std::string> granted =
        store.grantedPermissions(app, {"network.client"}, error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(granted, std::vector<std::string>({"network.client"}));
}

TEST_F(PermissionStoreTest, BindsEveryGrantToUserBundlePublisherAndPermissionVersion) {
    PermissionStore store(config());
    const PermissionSubject app = subject();
    std::string error;
    ASSERT_TRUE(store.setDecision(app, "network.client", true, error)) << error;

    EXPECT_FALSE(store.isGranted(subject("bundle-b"), "network.client", error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(store.isGranted(subject("bundle-a", 1001), "network.client", error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(store.isGranted(
        subject("bundle-a", 1000,
                "ed25519:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        "network.client", error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(store.isGranted(subject("bundle-a", 1000, "unverified", 2),
                                 "network.client", error));
    EXPECT_TRUE(error.empty());
}

TEST_F(PermissionStoreTest, PersistsAndRevokesKnownUndottedPermissions) {
    PermissionStore store(config());
    const PermissionSubject app = subject();
    std::string error;
    for (const char* permission : {"camera", "microphone", "notifications"}) {
        SCOPED_TRACE(permission);
        EXPECT_FALSE(store.isGranted(app, permission, error));
        ASSERT_TRUE(error.empty()) << error;
        ASSERT_TRUE(store.setDecision(app, permission, true, error)) << error;
    }
    PermissionStore reloaded(config());
    ASSERT_TRUE(reloaded.load(error)) << error;
    for (const char* permission : {"camera", "microphone", "notifications"}) {
        SCOPED_TRACE(permission);
        EXPECT_TRUE(reloaded.isGranted(app, permission, error)) << error;
        ASSERT_TRUE(reloaded.revoke(app, permission, error)) << error;
    }
    PermissionStore revoked(config());
    ASSERT_TRUE(revoked.load(error)) << error;
    EXPECT_EQ(revoked.size(), 0U);
    EXPECT_FALSE(revoked.setDecision(app, "org.lcl.unknown", true, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(PermissionStoreTest, RevocationReturnsToDefaultDenyAndDecisionUpdatesAreAtomic) {
    PermissionStore store(config());
    const PermissionSubject app = subject();
    std::string error;
    ASSERT_TRUE(store.setDecision(app, "network.client", true, error)) << error;
    ASSERT_TRUE(store.setDecision(app, "network.client", false, error)) << error;
    EXPECT_FALSE(store.isGranted(app, "network.client", error));
    EXPECT_TRUE(error.empty());

    ASSERT_TRUE(store.setDecision(app, "network.client", true, error)) << error;
    ASSERT_TRUE(store.revoke(app, "network.client", error)) << error;
    EXPECT_FALSE(store.isGranted(app, "network.client", error));
    EXPECT_TRUE(error.empty());

    PermissionStore reloaded(config());
    ASSERT_TRUE(reloaded.load(error)) << error;
    EXPECT_EQ(reloaded.size(), 0U);
}

TEST_F(PermissionStoreTest, RejectsMalformedOrRelaxedPersistentState) {
    const PermissionStoreConfig storeConfig = config();
    PermissionStore store(storeConfig);
    const PermissionSubject app = subject();
    std::string error;
    ASSERT_TRUE(store.setDecision(app, "network.client", true, error)) << error;
    chmod(storeConfig.storePath.c_str(), 0640);

    PermissionStore relaxed(storeConfig);
    EXPECT_FALSE(relaxed.load(error));
    EXPECT_NE(error.find("unsafe"), std::string::npos);

    chmod(storeConfig.storePath.c_str(), 0600);
    std::ofstream malformed(storeConfig.storePath, std::ios::trunc);
    malformed << "LCL_PERMISSION_STORE_V1\n1000 invalid "
              << hexEncodeDigest(sha256("bundle"))
              << " unverified network.client 1 grant\n";
    malformed.close();
    PermissionStore invalid(storeConfig);
    EXPECT_FALSE(invalid.load(error));
    EXPECT_NE(error.find("invalid"), std::string::npos);
}

} // namespace
} // namespace lcl::security
