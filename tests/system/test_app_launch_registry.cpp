#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "system/security/app_launch_registry.hpp"

namespace fs = std::filesystem;
using namespace lcl::security;

TEST(AppLaunchRegistryTest, RegistersFindsRemovesAndClearsLaunches) {
    const fs::path parent = fs::temp_directory_path() /
        ("lcl-launch-registry-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(fs::create_directories(parent));
    ASSERT_EQ(chmod(parent.c_str(), 0700), 0);
    const AppLaunchRegistryConfig config{
        .directoryPath = (parent / "launches").string(),
        .firstAppUid = 61000,
        .lastAppUid = 61999,
        .ownerUid = getuid(),
        .ownerGid = getgid(),
    };
    AppLaunchRegistry registry(config);
    std::string error;
    ASSERT_TRUE(registry.initializeAndReset(error)) << error;
    const AppLaunchIdentity expected{
        .instanceId = 41,
        .appId = "org.lcl.sandbox-test",
        .processGroupId = getpgrp(),
        .uid = 61000,
        .gid = 61000,
    };
    ASSERT_TRUE(registry.registerLaunch(expected, error)) << error;
    const auto loaded = registry.find(expected.instanceId, error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->appId, expected.appId);
    EXPECT_EQ(loaded->processGroupId, expected.processGroupId);
    EXPECT_FALSE(registry.registerLaunch(expected, error));
    EXPECT_TRUE(registry.remove(expected.instanceId, error)) << error;
    EXPECT_FALSE(registry.find(expected.instanceId, error));

    ASSERT_TRUE(registry.registerLaunch(expected, error)) << error;
    ASSERT_TRUE(registry.initializeAndReset(error)) << error;
    EXPECT_FALSE(registry.find(expected.instanceId, error));

    ASSERT_EQ(chmod(config.directoryPath.c_str(), 0770), 0);
    EXPECT_FALSE(registry.initializeAndReset(error));
    ASSERT_EQ(chmod(config.directoryPath.c_str(), 0700), 0);

    ASSERT_TRUE(fs::remove(config.directoryPath));
    const fs::path escape = parent / "escape";
    ASSERT_TRUE(fs::create_directory(escape));
    ASSERT_NO_THROW(fs::create_directory_symlink(escape, config.directoryPath));
    EXPECT_FALSE(registry.initializeAndReset(error));
    fs::remove_all(parent);
}
