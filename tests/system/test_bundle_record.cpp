#include <gtest/gtest.h>

#include "system/security/bundle_approval_store.hpp"
#include "system/security/bundle_record.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class BundleRecordTest : public ::testing::Test {
protected:
    fs::path temporaryDirectory_;

    void SetUp() override {
        temporaryDirectory_ = fs::temp_directory_path() /
                              ("lcl_bundle_record_" + std::to_string(getpid()));
        fs::remove_all(temporaryDirectory_);
        fs::create_directories(temporaryDirectory_);
        chmod(temporaryDirectory_.c_str(), 0700);
    }

    void TearDown() override {
        fs::remove_all(temporaryDirectory_);
    }

    fs::path createBundle(const std::string& name = "Example.app") {
        const fs::path bundle = temporaryDirectory_ / name;
        fs::create_directories(bundle / "Resources");
        fs::create_directories(bundle / "Executables");

        std::ofstream manifest(bundle / "Manifest.json");
        manifest << R"({"id":"org.lcl.example","name":"Example","version":"1.0.0","icon":"Resources/Icon.png","executable":"Executables/app","runtime":"org.lcl.native","requestedPermissions":["network.client","files.user-selected"]})";
        manifest.close();
        std::ofstream icon(bundle / "Resources" / "Icon.png");
        icon << "icon bytes";
        icon.close();
        std::ofstream executable(bundle / "Executables" / "app");
        executable << "#!/bin/lcl\n";
        executable.close();
        chmod((bundle / "Executables" / "app").c_str(), 0755);
        return bundle;
    }

    BundleApprovalStoreConfig approvalConfig() const {
        return BundleApprovalStoreConfig{
            (temporaryDirectory_ / "bundle-approvals.db").string(),
            getuid(),
            getgid(),
        };
    }
};

TEST_F(BundleRecordTest, RecordsEveryRegularFileInCanonicalOrder) {
    const fs::path bundle = createBundle();
    const auto metadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(metadata.has_value());

    std::string error;
    const auto record = makeBundleRecord(*metadata, error);

    ASSERT_TRUE(record.has_value()) << error;
    EXPECT_EQ(record->appId, "org.lcl.example");
    EXPECT_EQ(record->requestedPermissions,
              std::vector<std::string>({"files.user-selected", "network.client"}));
    ASSERT_EQ(record->files.size(), 3u);
    EXPECT_EQ(record->files[0].relativePath, "Executables/app");
    EXPECT_EQ(record->files[1].relativePath, "Manifest.json");
    EXPECT_EQ(record->files[2].relativePath, "Resources/Icon.png");
    EXPECT_FALSE(isZeroDigest(record->digest));
    EXPECT_TRUE(validateBundleRecord(*record, error)) << error;
}

TEST_F(BundleRecordTest, RejectsAManifestChangeAfterTheSchemaSnapshotWasParsed) {
    const fs::path bundle = createBundle();
    const auto metadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(metadata.has_value());

    std::string error;
    const auto original = makeBundleRecord(*metadata, error);
    ASSERT_TRUE(original.has_value()) << error;

    std::ofstream manifest(bundle / "Manifest.json", std::ios::trunc);
    manifest << R"({"id":"org.lcl.changed","name":"Changed","icon":"Resources/Icon.png","executable":"Executables/app"})";
    manifest.close();

    EXPECT_FALSE(makeBundleRecord(*metadata, error).has_value());
    EXPECT_NE(error.find("manifest changed"), std::string::npos);

    const auto reparsed = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(reparsed.has_value());
    const auto changed = makeBundleRecord(*reparsed, error);
    ASSERT_TRUE(changed.has_value()) << error;
    EXPECT_NE(changed->digest, original->digest);
}

TEST_F(BundleRecordTest, RejectsExtraSymbolicLinksAndHardLinks) {
    const fs::path bundle = createBundle();
    fs::create_symlink("Resources/Icon.png", bundle / "unexpected-link");
    const auto metadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(metadata.has_value());

    std::string error;
    EXPECT_FALSE(makeBundleRecord(*metadata, error).has_value());
    EXPECT_NE(error.find("symbolic link"), std::string::npos);

    fs::remove(bundle / "unexpected-link");
    std::ofstream unusedFile(bundle / "unexpected-file");
    unusedFile << "unused";
    unusedFile.close();
    fs::create_hard_link(bundle / "unexpected-file", bundle / "unexpected-hard-link");
    const auto reparsed = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_FALSE(makeBundleRecord(*reparsed, error).has_value());
    EXPECT_NE(error.find("hard link"), std::string::npos);
}

TEST_F(BundleRecordTest, ExcludesTheDetachedSignatureEnvelopeFromItsPayloadDigest) {
    const fs::path bundle = createBundle();
    std::string error;
    const auto unsignedMetadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(unsignedMetadata.has_value());
    const auto unsignedRecord = makeBundleRecord(*unsignedMetadata, error);
    ASSERT_TRUE(unsignedRecord.has_value()) << error;

    std::ofstream signature(bundle / "Signature.ed25519");
    signature << "detached signature envelope";
    signature.close();
    const auto signedMetadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(signedMetadata.has_value());
    const auto signedRecord = makeBundleRecord(*signedMetadata, error);

    ASSERT_TRUE(signedRecord.has_value()) << error;
    EXPECT_EQ(signedRecord->digest, unsignedRecord->digest);
    EXPECT_EQ(signedRecord->files.size(), unsignedRecord->files.size());
}

TEST_F(BundleRecordTest, ApprovalIsPerUserExactBundleDigestAndUnverifiedState) {
    const fs::path bundle = createBundle();
    const auto metadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(metadata.has_value());
    std::string error;
    const auto record = makeBundleRecord(*metadata, error);
    ASSERT_TRUE(record.has_value()) << error;

    BundleApprovalStore approvals(approvalConfig());
    constexpr uid_t kDesktopUser = 1000;
    EXPECT_FALSE(approvals.isApproved(kDesktopUser, *record, BundlePublisherState::Unverified, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(approvals.approve(kDesktopUser, *record, BundlePublisherState::Unverified, error)) << error;
    EXPECT_TRUE(approvals.isApproved(kDesktopUser, *record, BundlePublisherState::Unverified, error)) << error;
    EXPECT_TRUE(approvals.revoke(kDesktopUser, *record, BundlePublisherState::Unverified, error)) << error;
    EXPECT_FALSE(approvals.isApproved(kDesktopUser, *record, BundlePublisherState::Unverified, error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(approvals.approve(kDesktopUser, *record, BundlePublisherState::Unverified, error)) << error;
    EXPECT_FALSE(approvals.isApproved(kDesktopUser + 1, *record,
                                      BundlePublisherState::Unverified, error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(approvals.approve(kDesktopUser, *record,
                                   BundlePublisherState::SignatureVerified, error));

    std::ofstream icon(bundle / "Resources" / "Icon.png", std::ios::trunc);
    icon << "changed icon";
    icon.close();
    const auto changedMetadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(changedMetadata.has_value());
    const auto changedRecord = makeBundleRecord(*changedMetadata, error);
    ASSERT_TRUE(changedRecord.has_value()) << error;
    EXPECT_FALSE(approvals.isApproved(kDesktopUser, *changedRecord,
                                      BundlePublisherState::Unverified, error));
    EXPECT_TRUE(error.empty());
}

TEST_F(BundleRecordTest, RejectsApprovalStoreWithRelaxedPermissions) {
    const fs::path bundle = createBundle();
    const auto metadata = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(metadata.has_value());
    std::string error;
    const auto record = makeBundleRecord(*metadata, error);
    ASSERT_TRUE(record.has_value()) << error;

    const BundleApprovalStoreConfig config = approvalConfig();
    BundleApprovalStore approvals(config);
    ASSERT_TRUE(approvals.approve(1000, *record, BundlePublisherState::Unverified, error)) << error;
    chmod(config.storePath.c_str(), 0640);

    BundleApprovalStore reloaded(config);
    EXPECT_FALSE(reloaded.load(error));
    EXPECT_NE(error.find("unsafe"), std::string::npos);
}

} // namespace
} // namespace lcl::security
