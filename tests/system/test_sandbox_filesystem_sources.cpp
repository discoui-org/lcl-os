#include <gtest/gtest.h>

#include "system/security/sandbox_filesystem_sources.hpp"
#include "system/security/sandbox_landlock.hpp"
#include "system/security/session_user.hpp"

#include <filesystem>
#include <fstream>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    int compositorSocketDescriptor_{-1};
    int rasterSocketDescriptor_{-1};
    int compositorSocketFd_{-1};
    int rasterSocketFd_{-1};
    int executableDescriptor_{-1};
    int temporaryDescriptor_{-1};
    int deviceDescriptor_{-1};
    AppIdentity identity_;

    void SetUp() override {
        if (geteuid() != 0) {
            GTEST_SKIP() << "protected graphics socket ownership requires root";
        }
        root_ = fs::temp_directory_path() /
                ("lcl_sandbox_sources_" + std::to_string(getpid()));
        fs::create_directories(root_ / "App");
        fs::create_directories(root_ / "Data");
        fs::create_directories(root_ / "Cache");
        fs::create_directories(root_ / "Preferences");
        fs::create_directories(root_ / "Temporary");
        fs::create_directories(root_ / "Dev");
        auto makeRuntimeSocket = [this](const char* name, int& socketFd, int& descriptor) {
            const fs::path path = root_ / name;
            socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            ASSERT_GE(socketFd, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
            ASSERT_EQ(bind(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
            ASSERT_EQ(chown(path.c_str(), kSessionUserUid, kApplicationRuntimeGid), 0);
            ASSERT_EQ(chmod(path.c_str(), 0660), 0);
            descriptor = open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
            ASSERT_GE(descriptor, 0);
        };
        makeRuntimeSocket("compositor.sock", compositorSocketFd_, compositorSocketDescriptor_);
        makeRuntimeSocket("raster.sock", rasterSocketFd_, rasterSocketDescriptor_);
        std::ofstream executable(root_ / "App" / "app");
        executable << "sandbox executable";
        executable.close();
        ASSERT_EQ(chmod((root_ / "App" / "app").c_str(), 0700), 0);
        const uid_t appUid = getuid() == 0 ? 61003 : getuid();
        const gid_t appGid = getgid() == 0 ? 61003 : getgid();
        identity_ = {"org.lcl.sandbox-fs", appUid, appGid};
        for (const char* name : {"Data", "Cache", "Preferences", "Temporary"}) {
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
        executableDescriptor_ = open((root_ / "App" / "app").c_str(), O_RDONLY | O_CLOEXEC);
        temporaryDescriptor_ = open((root_ / "Temporary").c_str(),
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        deviceDescriptor_ = open((root_ / "Dev").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    void TearDown() override {
        for (const int descriptor : {appBundleDescriptor_, systemDescriptor_, dataDescriptor_,
                                     cacheDescriptor_, preferencesDescriptor_, temporaryDescriptor_,
                                     executableDescriptor_, deviceDescriptor_,
                                     compositorSocketDescriptor_, rasterSocketDescriptor_,
                                     compositorSocketFd_, rasterSocketFd_}) {
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
            .compositorSocketDescriptor = compositorSocketDescriptor_,
            .rasterSocketDescriptor = rasterSocketDescriptor_,
        };
    }

    SandboxLandlockRules validLandlockRules() const {
        return {
            .identity = identity_,
            .filesystemSources = validSources(),
            .executableDescriptor = executableDescriptor_,
            .temporaryDescriptor = temporaryDescriptor_,
            .deviceDescriptor = deviceDescriptor_,
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

TEST(SandboxFilesystemSourcesPathTest, AcceptsOnlySafeBundleRelativeExecutablePaths) {
    EXPECT_TRUE(isSafeSandboxBundleRelativePath("Executables/App"));
    EXPECT_TRUE(isSafeSandboxBundleRelativePath("app"));
    EXPECT_FALSE(isSafeSandboxBundleRelativePath(""));
    EXPECT_FALSE(isSafeSandboxBundleRelativePath("/host/executable"));
    EXPECT_FALSE(isSafeSandboxBundleRelativePath("Executables/../App"));
    EXPECT_FALSE(isSafeSandboxBundleRelativePath("Executables//App"));
    EXPECT_FALSE(isSafeSandboxBundleRelativePath("Executables\\App"));
}

TEST_F(SandboxFilesystemSourcesTest, LandlockRulesRequireOnlyPreparedPrivateDirectories) {
    if (geteuid() != 0) {
        GTEST_SKIP() << "root-owned device source is tested by target-system sandboxd";
    }
    std::string error;
    EXPECT_TRUE(validateSandboxLandlockRules(validLandlockRules(), error)) << error;

    SandboxLandlockRules unsafe = validLandlockRules();
    unsafe.deviceDescriptor = dataDescriptor_;
    EXPECT_FALSE(validateSandboxLandlockRules(unsafe, error));
    EXPECT_EQ(error, "sandbox device source is not a root-controlled directory");
}

} // namespace
} // namespace lcl::security
