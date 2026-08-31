#include <gtest/gtest.h>

#include "system/security/bundle_record.hpp"
#include "system/security/bundle_snapshot_store.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class BundleSnapshotStoreTest : public ::testing::Test {
protected:
    fs::path temporaryDirectory_;
    fs::path sourceDirectory_;
    fs::path securityDirectory_;

    void SetUp() override {
        temporaryDirectory_ = fs::temp_directory_path() /
                              ("lcl_bundle_snapshot_" + std::to_string(getpid()));
        fs::remove_all(temporaryDirectory_);
        sourceDirectory_ = temporaryDirectory_ / "Applications";
        securityDirectory_ = temporaryDirectory_ / "security";
        fs::create_directories(sourceDirectory_);
        fs::create_directories(securityDirectory_);
        chmod(temporaryDirectory_.c_str(), 0700);
        chmod(securityDirectory_.c_str(), 0700);
    }

    void TearDown() override { fs::remove_all(temporaryDirectory_); }

    fs::path createBundle() const {
        const fs::path bundle = sourceDirectory_ / "Example.app";
        fs::create_directories(bundle / "Executables");
        fs::create_directories(bundle / "Resources");
        std::ofstream manifest(bundle / "Manifest.json");
        manifest << R"({"id":"org.lcl.snapshot","name":"Snapshot","version":"1.0","icon":"Resources/Icon.png","executable":"Executables/app","runtime":"org.lcl.native"})";
        manifest.close();
        std::ofstream icon(bundle / "Resources" / "Icon.png");
        icon << "icon";
        icon.close();
        std::ofstream executable(bundle / "Executables" / "app");
        executable << "#!/bin/sh\nexit 0\n";
        executable.close();
        chmod((bundle / "Executables" / "app").c_str(), 0755);
        return bundle;
    }

    BundleSnapshotStore store() const {
        return BundleSnapshotStore({
            .snapshotsRoot = (securityDirectory_ / "bundles").string(),
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        });
    }
};

TEST_F(BundleSnapshotStoreTest, StagesAnImmutableRecordBoundBundle) {
    const fs::path bundle = createBundle();
    const auto source = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(source.has_value());
    std::string error;
    const auto record = makeBundleRecord(*source, error);
    ASSERT_TRUE(record.has_value()) << error;

    lcl::core::AppBundleMetadata snapshot;
    auto snapshotStore = store();
    ASSERT_TRUE(snapshotStore.stage(*source, *record, snapshot, error)) << error;
    EXPECT_EQ(snapshot.appId, source->appId);
    EXPECT_NE(snapshot.bundlePath, source->bundlePath);

    struct stat status {};
    ASSERT_EQ(stat(snapshot.bundlePath.c_str(), &status), 0);
    EXPECT_EQ(status.st_uid, getuid());
    EXPECT_EQ(status.st_gid, getgid());
    EXPECT_EQ(status.st_mode & 0022, 0);
    ASSERT_TRUE(snapshot.executableHandle);
    EXPECT_TRUE(snapshot.executableHandle->valid());

    std::ofstream changed(bundle / "Executables" / "app", std::ios::trunc);
    changed << "changed";
    changed.close();
    const auto stagedRecord = makeBundleRecord(snapshot, error);
    ASSERT_TRUE(stagedRecord.has_value()) << error;
    EXPECT_EQ(stagedRecord->payloadDigest, record->payloadDigest);
}

TEST_F(BundleSnapshotStoreTest, RejectsAChangedSourceBeforeItCommits) {
    const fs::path bundle = createBundle();
    const auto source = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(source.has_value());
    std::string error;
    const auto record = makeBundleRecord(*source, error);
    ASSERT_TRUE(record.has_value()) << error;

    std::ofstream changed(bundle / "Resources" / "Icon.png", std::ios::trunc);
    changed << "changed icon";
    changed.close();

    lcl::core::AppBundleMetadata snapshot;
    auto snapshotStore = store();
    EXPECT_FALSE(snapshotStore.stage(*source, *record, snapshot, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(BundleSnapshotStoreTest, PreservesTheDetachedSignatureEnvelopeInTheRecordIdentity) {
    const fs::path bundle = createBundle();
    std::ofstream signature(bundle / "Signature.ed25519", std::ios::binary);
    signature << "unverified signature envelope";
    signature.close();
    const auto source = lcl::core::AppBundleParser::parseBundle(bundle.string());
    ASSERT_TRUE(source.has_value());
    std::string error;
    const auto record = makeBundleRecord(*source, error);
    ASSERT_TRUE(record.has_value()) << error;
    ASSERT_FALSE(isZeroDigest(record->signatureEnvelopeDigest));

    lcl::core::AppBundleMetadata snapshot;
    auto snapshotStore = store();
    ASSERT_TRUE(snapshotStore.stage(*source, *record, snapshot, error)) << error;
    const auto stagedRecord = makeBundleRecord(snapshot, error);
    ASSERT_TRUE(stagedRecord.has_value()) << error;
    EXPECT_EQ(stagedRecord->digest, record->digest);
    EXPECT_TRUE(fs::is_regular_file(fs::path(snapshot.bundlePath) / "Signature.ed25519"));
}

} // namespace
} // namespace lcl::security
