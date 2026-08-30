#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "system/security/app_identity_registry.hpp"

namespace fs = std::filesystem;
using namespace lcl::security;

class AppIdentityRegistryTest : public ::testing::Test {
protected:
    fs::path tempDir;
    AppIdentityRegistryConfig config;

    void SetUp() override {
        tempDir = fs::temp_directory_path() /
            ("lcl-identity-registry-" + std::to_string(getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tempDir);
        chmod(tempDir.c_str(), 0700);
        config = {
            .registryPath = (tempDir / "identities.v1").string(),
            .firstAppUid = 61000,
            .lastAppUid = 61001,
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        };
    }

    void TearDown() override { fs::remove_all(tempDir); }
};

TEST_F(AppIdentityRegistryTest, AllocatesStableAndDistinctIdentities) {
    AppIdentityRegistry registry(config);
    std::string error;

    const auto first = registry.getOrCreate("org.lcl.first", error);
    ASSERT_TRUE(first.has_value()) << error;
    EXPECT_EQ(first->uid, 61000u);
    EXPECT_EQ(first->gid, 61000u);
    const auto sameFirst = registry.getOrCreate("org.lcl.first", error);
    ASSERT_TRUE(sameFirst.has_value()) << error;
    EXPECT_EQ(sameFirst->uid, first->uid);

    const auto second = registry.getOrCreate("org.lcl.second", error);
    ASSERT_TRUE(second.has_value()) << error;
    EXPECT_EQ(second->uid, 61001u);
    EXPECT_EQ(registry.size(), 2u);

    struct stat status {};
    ASSERT_EQ(stat(config.registryPath.c_str(), &status), 0);
    EXPECT_EQ(status.st_uid, getuid());
    EXPECT_EQ(status.st_gid, getgid());
    EXPECT_EQ(status.st_mode & 0077, 0);

    AppIdentityRegistry reloaded(config);
    ASSERT_TRUE(reloaded.load(error)) << error;
    ASSERT_TRUE(reloaded.find("org.lcl.first").has_value());
    EXPECT_EQ(reloaded.find("org.lcl.first")->uid, first->uid);
    EXPECT_EQ(reloaded.find("org.lcl.second")->uid, second->uid);
}

TEST_F(AppIdentityRegistryTest, RejectsInvalidIdsAndExhaustion) {
    AppIdentityRegistry registry(config);
    std::string error;

    EXPECT_FALSE(registry.getOrCreate("Org.Lcl.Invalid", error).has_value());
    EXPECT_EQ(error, "invalid canonical app ID");
    EXPECT_TRUE(registry.getOrCreate("org.lcl.first", error).has_value()) << error;
    EXPECT_TRUE(registry.getOrCreate("org.lcl.second", error).has_value()) << error;
    EXPECT_FALSE(registry.getOrCreate("org.lcl.third", error).has_value());
    EXPECT_EQ(error, "app identity UID range is exhausted");
}

TEST_F(AppIdentityRegistryTest, RejectsRegistryWithUnsafePermissionsOrRecords) {
    AppIdentityRegistry registry(config);
    std::string error;
    ASSERT_TRUE(registry.getOrCreate("org.lcl.safe", error)) << error;

    chmod(config.registryPath.c_str(), 0644);
    AppIdentityRegistry worldReadable(config);
    EXPECT_FALSE(worldReadable.load(error));

    chmod(config.registryPath.c_str(), 0600);
    std::ofstream malformed(config.registryPath, std::ios::trunc);
    malformed << "LCL_APP_IDENTITIES_V1\n"
              << "org.lcl.first 61000 61000\n"
              << "org.lcl.second 61000 61000\n";
    malformed.close();

    AppIdentityRegistry duplicateUid(config);
    EXPECT_FALSE(duplicateUid.load(error));
}
