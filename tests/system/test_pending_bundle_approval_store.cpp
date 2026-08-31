#include <gtest/gtest.h>

#include "system/security/bundle_approval_authority.hpp"
#include "system/security/pending_bundle_approval_store.hpp"

#include <chrono>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class PendingBundleApprovalStoreTest : public ::testing::Test {
protected:
    fs::path temporaryDirectory_;
    BundleRecord record_;

    void SetUp() override {
        temporaryDirectory_ = fs::temp_directory_path() /
                              ("lcl_pending_bundle_approval_" +
                               std::to_string(getpid()) + "_" +
                               std::to_string(
                                   std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temporaryDirectory_);
        chmod(temporaryDirectory_.c_str(), 0700);

        record_.appId = "org.lcl.pending";
        record_.appVersion = "1.0";
        record_.type = "gui";
        record_.runtime = "org.lcl.native";
        record_.files = {BundleFileDigest{"Manifest.json", 0644, sha256("manifest")}};
        record_.payloadDigest = digestBundlePayload(record_);
        record_.digest = digestBundleRecord(record_);
    }

    void TearDown() override { fs::remove_all(temporaryDirectory_); }

    PendingBundleApprovalStoreConfig config() const {
        return {
            .storePath = (temporaryDirectory_ / "pending-approvals.v1").string(),
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        };
    }
};

TEST_F(PendingBundleApprovalStoreTest,
       PersistsAnExactRequestAndUpdatesOnlyItsPresentationLocation) {
    std::string error;
    PendingBundleApprovalStore store(config());
    ASSERT_TRUE(store.record(1000, record_, BundlePublisherState::Unverified,
                             BundleSourceScope::Direct,
                             "/Users/Rei/Downloads/Example.app", "Example", error))
        << error;

    PendingBundleApprovalStore reloaded(config());
    std::vector<PendingBundleApproval> pending = reloaded.pendingFor(1000, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().appId, record_.appId);
    EXPECT_EQ(pending.front().bundleRecordDigest, record_.digest);
    EXPECT_EQ(pending.front().displayName, "Example");
    EXPECT_EQ(pending.front().appVersion, "1.0");
    EXPECT_EQ(pending.front().sourceScope, BundleSourceScope::Direct);

    ASSERT_TRUE(store.record(1000, record_, BundlePublisherState::Unverified,
                             BundleSourceScope::User,
                             "/Users/Rei/Applications/Example.app", "Example", error))
        << error;
    pending = reloaded.pendingFor(1000, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().sourceScope, BundleSourceScope::User);
    EXPECT_EQ(pending.front().bundlePath, "/Users/Rei/Applications/Example.app");

    ASSERT_TRUE(store.remove(1000, record_, BundlePublisherState::Unverified, error)) << error;
    EXPECT_TRUE(reloaded.pendingFor(1000, error).empty());
    EXPECT_TRUE(error.empty()) << error;
}

TEST_F(PendingBundleApprovalStoreTest, RejectsPrivilegedOrUnsafePendingRecords) {
    std::string error;
    PendingBundleApprovalStore store(config());
    EXPECT_FALSE(store.record(1000, record_, BundlePublisherState::SignatureVerified,
                              BundleSourceScope::Direct,
                              "/Users/Rei/Downloads/Example.app", "Example", error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(store.record(1000, record_, BundlePublisherState::Unverified,
                              BundleSourceScope::System,
                              "/System/Applications/Example.app", "Example", error));
    EXPECT_FALSE(error.empty());

    error.clear();
    ASSERT_TRUE(store.record(1000, record_, BundlePublisherState::Unverified,
                             BundleSourceScope::Direct,
                             "/Users/Rei/Downloads/Example.app", "Example", error))
        << error;
    chmod(config().storePath.c_str(), 0640);
    PendingBundleApprovalStore reloaded(config());
    EXPECT_TRUE(reloaded.pendingFor(1000, error).empty());
    EXPECT_NE(error.find("unsafe"), std::string::npos);
}

TEST_F(PendingBundleApprovalStoreTest,
       ApprovalAuthorityRequiresAPendingExactRecordThenClearsIt) {
    std::string error;
    BundleApprovalAuthority authority({
        .approvals = {
            .storePath = (temporaryDirectory_ / "approvals.v1").string(),
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        },
        .pending = config(),
    });
    BundleRecord changedRecord = record_;
    changedRecord.appVersion = "2.0";
    changedRecord.payloadDigest = digestBundlePayload(changedRecord);
    changedRecord.digest = digestBundleRecord(changedRecord);
    EXPECT_FALSE(authority.approvePending(1000, changedRecord.appId,
                                          changedRecord.digest, error));
    EXPECT_NE(error.find("not pending"), std::string::npos);

    error.clear();
    PendingBundleApprovalStore pending(config());
    ASSERT_TRUE(pending.record(1000, record_, BundlePublisherState::Unverified,
                               BundleSourceScope::Direct,
                               "/Users/Rei/Downloads/Example.app", "Example", error))
        << error;

    ASSERT_TRUE(authority.approvePending(1000, record_.appId, record_.digest, error)) << error;
    EXPECT_TRUE(authority.pendingFor(1000, error).empty());
    EXPECT_TRUE(error.empty()) << error;

    BundleApprovalStore approvals({
        .storePath = (temporaryDirectory_ / "approvals.v1").string(),
        .ownerUid = getuid(),
        .ownerGid = getgid(),
    });
    EXPECT_TRUE(approvals.isApproved(1000, record_, BundlePublisherState::Unverified, error))
        << error;
    EXPECT_TRUE(authority.revoke(1000, record_.appId, record_.digest, error)) << error;
    EXPECT_FALSE(approvals.isApproved(1000, record_, BundlePublisherState::Unverified, error));
    EXPECT_TRUE(error.empty()) << error;
}

} // namespace
} // namespace lcl::security
