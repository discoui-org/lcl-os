#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "system/security/app_data_store.hpp"

namespace fs = std::filesystem;
using namespace lcl::security;

class AppDataStoreTest : public ::testing::Test {
protected:
    fs::path tempDir;
    fs::path containersRoot;
    AppDataStoreConfig config;

    AppIdentity testIdentity() const {
        const uid_t appUid = getuid() == 0 ? 61000 : getuid();
        const gid_t appGid = getgid() == 0 ? 61000 : getgid();
        return {"org.lcl.storage", appUid, appGid};
    }

    void SetUp() override {
        tempDir = fs::temp_directory_path() /
            ("lcl-app-data-store-" + std::to_string(getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        containersRoot = tempDir / "Containers";
        fs::create_directories(containersRoot);
        chmod(tempDir.c_str(), 0700);
        chmod(containersRoot.c_str(), 0700);
        config = {
            .containersRoot = containersRoot.string(),
            .ownerUid = getuid(),
            .ownerGid = getgid(),
        };
    }

    void TearDown() override { fs::remove_all(tempDir); }
};

TEST_F(AppDataStoreTest, CreatesAppOwnedPersistentDirectories) {
    AppDataStore store(config);
    AppPersistentDirectories directories;
    std::string error;
    const AppIdentity identity = testIdentity();

    ASSERT_TRUE(store.ensurePersistentDirectories(identity, directories, error)) << error;
    EXPECT_EQ(directories.container, (containersRoot / "org.lcl.storage").string());
    EXPECT_EQ(directories.data, (containersRoot / "org.lcl.storage" / "Data").string());
    EXPECT_EQ(directories.cache, (containersRoot / "org.lcl.storage" / "Cache").string());
    EXPECT_EQ(directories.preferences,
              (containersRoot / "org.lcl.storage" / "Preferences").string());

    for (const std::string& directory :
         {directories.container, directories.data, directories.cache, directories.preferences}) {
        struct stat status {};
        ASSERT_EQ(stat(directory.c_str(), &status), 0);
        EXPECT_TRUE(S_ISDIR(status.st_mode));
        EXPECT_EQ(status.st_uid, identity.uid);
        EXPECT_EQ(status.st_gid, identity.gid);
        EXPECT_EQ(status.st_mode & 0077, 0);
    }
    EXPECT_FALSE(fs::exists(fs::path(directories.container) / "Temporary"));
}

TEST_F(AppDataStoreTest, RejectsUnsafeContainerRootAndAppIdentity) {
    AppDataStore store(config);
    AppPersistentDirectories directories;
    std::string error;
    const AppIdentity identity = testIdentity();
    EXPECT_FALSE(store.ensurePersistentDirectories({"Org.Lcl.Invalid", identity.uid, identity.gid},
                                                   directories, error));
    EXPECT_EQ(error, "invalid app identity for persistent storage");

    chmod(containersRoot.c_str(), 0777);
    EXPECT_FALSE(store.ensurePersistentDirectories(identity, directories, error));
    EXPECT_EQ(error, "app containers root is not an owner-controlled directory");
}

TEST_F(AppDataStoreTest, RejectsAContainerSymlink) {
    AppDataStore store(config);
    AppPersistentDirectories directories;
    std::string error;
    const fs::path outside = tempDir / "outside";
    fs::create_directories(outside);
    fs::create_symlink(outside, containersRoot / "org.lcl.storage");

    EXPECT_FALSE(store.ensurePersistentDirectories(testIdentity(), directories, error));
    EXPECT_NE(error.find("not a safe directory"), std::string::npos);
}

TEST_F(AppDataStoreTest, RepairsPersistentDirectoryPermissions) {
    AppDataStore store(config);
    AppPersistentDirectories directories;
    std::string error;
    const AppIdentity identity = testIdentity();
    ASSERT_TRUE(store.ensurePersistentDirectories(identity, directories, error)) << error;
    ASSERT_EQ(chmod(directories.cache.c_str(), 0777), 0);

    ASSERT_TRUE(store.ensurePersistentDirectories(identity, directories, error)) << error;
    struct stat status {};
    ASSERT_EQ(stat(directories.cache.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0077, 0);
}
