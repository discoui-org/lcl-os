#include <gtest/gtest.h>

#include "system/security/sandbox_filesystem_sources.hpp"

#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class SandboxFilesystemSourcesTest : public ::testing::Test {
protected:
    fs::path root_;
    int appBundleDescriptor_{-1};
    int systemDescriptor_{-1};
    int dataDescriptor_{-1};
    int cacheDescriptor_{-1};
    int preferencesDescriptor_{-1};
    AppIdentity identity_;

    void SetUp() override {
        if (getuid() != 0 && getuid() != getgid()) {
            GTEST_SKIP() << "test identity requires a matching UID/GID";
        }
        root_ = fs::temp_directory_path() /
                ("lcl_sandbox_sources_" + std::to_string(getpid()));
        fs::create_directories(root_ / "App");
        fs::create_directories(root_ / "Data");
        fs::create_directories(root_ / "Cache");
        fs::create_directories(root_ / "Preferences");
        const uid_t appUid = getuid() == 0 ? 61003 : getuid();
        const gid_t appGid = getgid() == 0 ? 61003 : getgid();
        identity_ = {"org.lcl.sandbox-fs", appUid, appGid};
        for (const char* name : {"Data", "Cache", "Preferences"}) {
            const fs::path directory = root_ / name;
            ASSERT_EQ(chown(directory.c_str(), appUid, appGid), 0);
            ASSERT_EQ(chmod(directory.c_str(), 0700), 0);
        }
        appBundleDescriptor_ = open((root_ / "App").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        systemDescriptor_ = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dataDescriptor_ = open((root_ / "Data").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        cacheDescriptor_ = open((root_ / "Cache").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        preferencesDescriptor_ = open((root_ / "Preferences").c_str(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    void TearDown() override {
        for (const int descriptor : {appBundleDescriptor_, systemDescriptor_, dataDescriptor_,
                                     cacheDescriptor_, preferencesDescriptor_}) {
            if (descriptor >= 0) {
                close(descriptor);
            }
        }
        fs::remove_all(root_);
    }

    SandboxFilesystemSources validSources() const {
        return {
            .appBundleDescriptor = appBundleDescriptor_,
            .systemDescriptor = systemDescriptor_,
            .dataDescriptor = dataDescriptor_,
            .cacheDescriptor = cacheDescriptor_,
            .preferencesDescriptor = preferencesDescriptor_,
        };
    }
};

TEST_F(SandboxFilesystemSourcesTest, AcceptsOnlyProtectedMountSources) {
    ASSERT_GE(appBundleDescriptor_, 0);
    ASSERT_GE(systemDescriptor_, 0);
    std::string error;
    EXPECT_TRUE(validateSandboxFilesystemSources(validSources(), identity_, error)) << error;
}

TEST_F(SandboxFilesystemSourcesTest, RejectsAStorageDirectoryWithLoosePermissions) {
    ASSERT_EQ(chmod((root_ / "Cache").c_str(), 0755), 0);
    std::string error;
    EXPECT_FALSE(validateSandboxFilesystemSources(validSources(), identity_, error));
    EXPECT_NE(error.find("app-owned 0700"), std::string::npos);
}

} // namespace
} // namespace lcl::security
