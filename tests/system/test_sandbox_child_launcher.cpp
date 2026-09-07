#include <gtest/gtest.h>

#include "system/security/sandbox_child_launcher.hpp"
#include "system/security/sandbox_launch_material.hpp"
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

class SandboxChildLauncherTest : public ::testing::Test {
protected:
    fs::path executablePath_;
    int executableDescriptor_{-1};
    int appBundleDescriptor_{-1};
    int systemDescriptor_{-1};
    int dataDescriptor_{-1};
    int cacheDescriptor_{-1};
    int preferencesDescriptor_{-1};
    int compositorSocketDescriptor_{-1};
    int rasterSocketDescriptor_{-1};
    int compositorSocketFd_{-1};
    int rasterSocketFd_{-1};

    void SetUp() override {
        if (geteuid() != 0) {
            GTEST_SKIP() << "protected graphics socket ownership requires root";
        }
        const fs::path directory = fs::temp_directory_path() /
                                   ("lcl_sandbox_child_" + std::to_string(getpid()));
        fs::create_directories(directory);
        auto makeRuntimeSocket = [&directory](const char* name, int& socketFd, int& descriptor) {
            const fs::path path = directory / name;
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
        executablePath_ = directory / "app";
        std::ofstream executable(executablePath_);
        executable << "#!/bin/sh\nexit 0\n";
        executable.close();
        chmod(executablePath_.c_str(), 0700);
        const uid_t appUid = getuid() == 0 ? 61000 : getuid();
        const gid_t appGid = getgid() == 0 ? 61000 : getgid();
        for (const char* name : {"Data", "Cache", "Preferences"}) {
            const fs::path storage = directory / name;
            fs::create_directories(storage);
            ASSERT_EQ(chown(storage.c_str(), appUid, appGid), 0);
            ASSERT_EQ(chmod(storage.c_str(), 0700), 0);
        }
        executableDescriptor_ = open(executablePath_.c_str(), O_RDONLY | O_CLOEXEC);
        appBundleDescriptor_ = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        systemDescriptor_ = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dataDescriptor_ = open((directory / "Data").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        cacheDescriptor_ = open((directory / "Cache").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        preferencesDescriptor_ = open((directory / "Preferences").c_str(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    void TearDown() override {
        for (const int descriptor : {executableDescriptor_, appBundleDescriptor_, systemDescriptor_,
                                     dataDescriptor_, cacheDescriptor_, preferencesDescriptor_}) {
            if (descriptor >= 0) {
                close(descriptor);
            }
        }
        for (const int descriptor : {compositorSocketDescriptor_, rasterSocketDescriptor_,
                                     compositorSocketFd_, rasterSocketFd_}) {
            if (descriptor >= 0) close(descriptor);
        }
        fs::remove_all(executablePath_.parent_path());
    }

    SandboxChildLaunchSpec validSpec() const {
        const uid_t appUid = getuid() == 0 ? 61000 : getuid();
        const gid_t appGid = getgid() == 0 ? 61000 : getgid();
        VerifiedApplication application{};
        application.appId = "org.lcl.sandbox-child";
        application.identity = {application.appId, appUid, appGid};
        application.runtime = SandboxRuntime::Native;
        application.bundleRecordDigest = sha256("bundle-record");
        std::string error;
        const auto plan = makeThirdPartySandboxLaunchPlan(application, {}, 42, error);
        EXPECT_TRUE(plan.has_value()) << error;

        SandboxChildLaunchSpec spec{};
        spec.plan = plan.value_or(SandboxLaunchPlan{});
        spec.hardening.privatePidNamespace = false;
        spec.hardening.privateIpcNamespace = false;
        spec.hardening.requireLandlock = false;
        spec.hardening.requireCgroupResourceAccounting = false;
        spec.identity = application.identity;
        spec.filesystemSources = {
            .appBundleDescriptor = appBundleDescriptor_,
            .systemDescriptor = systemDescriptor_,
            .dataDescriptor = dataDescriptor_,
            .cacheDescriptor = cacheDescriptor_,
            .preferencesDescriptor = preferencesDescriptor_,
            .compositorSocketDescriptor = compositorSocketDescriptor_,
            .rasterSocketDescriptor = rasterSocketDescriptor_,
        };
        spec.executableBundlePath = "app";
        spec.executableDescriptor = executableDescriptor_;
        spec.arguments = {"--safe"};
        return spec;
    }
};

TEST_F(SandboxChildLauncherTest, AcceptsProtectedLaunchMaterialBeforeRuntimeKernelSetup) {
    ASSERT_GE(executableDescriptor_, 0);
    ASSERT_GE(appBundleDescriptor_, 0);
    ASSERT_GE(systemDescriptor_, 0);
    SandboxChildLaunchSpec spec = validSpec();
    std::string error;
    EXPECT_TRUE(validateSandboxChildLaunchSpec(spec, error)) << error;

    spec.cgroup = SandboxCgroupBinding{
        .manager = nullptr,
        .cgroup = {.instanceId = spec.plan.request.instanceId, .path = "/unsafe/cgroup"},
    };
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));
    EXPECT_NE(error.find("unsafe"), std::string::npos);
}

TEST_F(SandboxChildLauncherTest, RejectsUnsafeArgumentsAndNonExecutableDescriptors) {
    ASSERT_GE(executableDescriptor_, 0);
    SandboxChildLaunchSpec spec = validSpec();
    std::string error;
    spec.arguments = {std::string("bad\0argument", 12)};
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));

    spec = validSpec();
    chmod(executablePath_.c_str(), 0600);
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));

    spec = validSpec();
    spec.filesystemSources.dataDescriptor = -1;
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));

    spec = validSpec();
    spec.executableBundlePath = "../outside";
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));
}

TEST_F(SandboxChildLauncherTest, RefusesToSpawnWhenTheCallerIsNotRoot) {
    ASSERT_GE(executableDescriptor_, 0);
    if (geteuid() == 0) {
        GTEST_SKIP() << "root-only launcher refusal is covered by target-system acceptance";
    }
    const SandboxLaunchResult result = spawnSandboxChild(validSpec());
    EXPECT_EQ(result.status, SandboxLaunchStatus::SetupFailed);
    EXPECT_NE(result.message.find("requires root"), std::string::npos);
}

TEST_F(SandboxChildLauncherTest, RetainsOnlyPolicyBoundVerifiedLaunchMaterial) {
    ASSERT_GE(executableDescriptor_, 0);
    SandboxChildLaunchSpec protectedSpec = validSpec();
    ASSERT_TRUE(protectedSpec.plan.request.instanceId != 0);

    VerifiedApplication application{};
    application.appId = protectedSpec.identity.appId;
    application.identity = protectedSpec.identity;
    application.runtime = protectedSpec.plan.profile.runtime;
    application.bundleRecordDigest = protectedSpec.plan.request.bundleRecordDigest;
    SandboxLaunchMaterialInput input{
        .application = application,
        .filesystemSources = protectedSpec.filesystemSources,
        .executableBundlePath = protectedSpec.executableBundlePath,
        .executableDescriptor = protectedSpec.executableDescriptor,
        .arguments = protectedSpec.arguments,
    };

    SandboxLaunchMaterialRegistry registry;
    std::string error;
    ASSERT_TRUE(registry.registerMaterial(input, error)) << error;
    EXPECT_TRUE(registry.hasMaterialFor(protectedSpec.plan));

    SandboxLaunchMaterialInput mismatchedPath = input;
    mismatchedPath.executableBundlePath = "Data";
    SandboxLaunchMaterialRegistry mismatchRegistry;
    EXPECT_FALSE(mismatchRegistry.registerMaterial(mismatchedPath, error));
    EXPECT_NE(error.find("unsafe"), std::string::npos);

    SandboxCgroupManager manager;
    SandboxPlatformHardening linuxHardening{};
    SandboxChildLaunchSpec rebuilt{};
    const SandboxCgroup cgroup{.instanceId = protectedSpec.plan.request.instanceId,
                                .path = "/sys/fs/cgroup/lcl/apps/instance-42"};
    const SandboxCgroupBinding cgroupBinding{.manager = &manager, .cgroup = cgroup};
    ASSERT_TRUE(registry.makeChildLaunchSpec(protectedSpec.plan, linuxHardening, cgroupBinding,
                                             rebuilt, error))
        << error;
    ASSERT_TRUE(rebuilt.cgroup.has_value());
    EXPECT_EQ(rebuilt.cgroup->manager, &manager);
    EXPECT_EQ(rebuilt.cgroup->cgroup.path, cgroup.path);
    EXPECT_NE(rebuilt.executableDescriptor, protectedSpec.executableDescriptor);
    EXPECT_TRUE(validateSandboxChildLaunchSpec(rebuilt, error)) << error;

    SandboxLaunchPlan changedPlan = protectedSpec.plan;
    changedPlan.request.bundleRecordDigest = sha256("different-bundle-record");
    EXPECT_FALSE(registry.hasMaterialFor(changedPlan));
    EXPECT_FALSE(registry.makeChildLaunchSpec(changedPlan, linuxHardening, cgroupBinding,
                                              rebuilt, error));
    EXPECT_NE(error.find("unavailable"), std::string::npos);
}

} // namespace
} // namespace lcl::security
