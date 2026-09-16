#include <gtest/gtest.h>

#include "system/security/sandbox_child_launcher.hpp"
#include "system/security/sandbox_launch_material.hpp"
#include "system/security/session_user.hpp"
#include "system/security/system_permission_profile.hpp"

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
    int gpuSocketDescriptor_{-1};
    int compositorSocketFd_{-1};
    int rasterSocketFd_{-1};
    int gpuSocketFd_{-1};
    int replacementCompositorSocketFd_{-1};

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
        makeRuntimeSocket("gpu.sock", gpuSocketFd_, gpuSocketDescriptor_);
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
                                     gpuSocketDescriptor_, compositorSocketFd_, rasterSocketFd_,
                                     gpuSocketFd_,
                                     replacementCompositorSocketFd_}) {
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

TEST_F(SandboxChildLauncherTest, DeniesGpuEndpointWithoutGraphicsGpuCapability) {
    SandboxChildLaunchSpec spec = validSpec();
    spec.filesystemSources.gpuSocketDescriptor = gpuSocketDescriptor_;

    std::string error;
    EXPECT_FALSE(validateSandboxChildLaunchSpec(spec, error));
    EXPECT_EQ(error, "sandbox graphics.gpu capability and broker endpoint disagree");
}

TEST_F(SandboxChildLauncherTest, GivesGpuEndpointOnlyToAnEffectiveCapability) {
    SandboxChildLaunchSpec seed = validSpec();
    VerifiedApplication application{};
    application.appId = seed.identity.appId;
    application.identity = seed.identity;
    application.runtime = SandboxRuntime::Native;
    application.requestedPermissions = {"graphics.gpu"};
    application.bundleRecordDigest = seed.plan.request.bundleRecordDigest;

    std::string error;
    const auto deniedPlan = makeThirdPartySandboxLaunchPlan(application, {}, 43, error);
    ASSERT_TRUE(deniedPlan.has_value()) << error;
    const auto grantedPlan = makeThirdPartySandboxLaunchPlan(
        application, {"graphics.gpu"}, 44, error);
    ASSERT_TRUE(grantedPlan.has_value()) << error;

    SandboxLaunchMaterialInput input{
        .application = application,
        .filesystemSources = seed.filesystemSources,
        .executableBundlePath = seed.executableBundlePath,
        .executableDescriptor = seed.executableDescriptor,
        .arguments = seed.arguments,
    };
    input.filesystemSources.gpuSocketDescriptor = gpuSocketDescriptor_;
    SandboxLaunchMaterialRegistry registry;
    ASSERT_TRUE(registry.registerMaterial(input, error)) << error;

    SandboxPlatformHardening hardening{};
    hardening.requireCgroupResourceAccounting = false;
    SandboxChildLaunchSpec deniedSpec{};
    ASSERT_TRUE(registry.makeChildLaunchSpec(*deniedPlan, hardening, std::nullopt,
                                             deniedSpec, error)) << error;
    EXPECT_EQ(deniedSpec.filesystemSources.gpuSocketDescriptor, -1);

    SandboxChildLaunchSpec grantedSpec{};
    ASSERT_TRUE(registry.makeChildLaunchSpec(*grantedPlan, hardening, std::nullopt,
                                             grantedSpec, error)) << error;
    EXPECT_GE(grantedSpec.filesystemSources.gpuSocketDescriptor, 0);
    EXPECT_TRUE(validateSandboxChildLaunchSpec(grantedSpec, error)) << error;
}

TEST_F(SandboxChildLauncherTest, QtSmokeGpuGrantControlsPrivateBrokerEndpoint) {
    SandboxChildLaunchSpec seed = validSpec();
    VerifiedApplication application{};
    application.appId = "org.lcl.qt.smoke";
    application.identity = {application.appId, seed.identity.uid, seed.identity.gid};
    application.runtime = SandboxRuntime::Native;
    application.requestedPermissions = {"graphics.gpu", "graphics.render-node"};
    application.bundleRecordDigest = seed.plan.request.bundleRecordDigest;

    const auto systemGrants = staticSystemImagePermissionGrants(
        application.appId, application.requestedPermissions);
    EXPECT_EQ(systemGrants,
              std::vector<std::string>({"graphics.gpu", "graphics.render-node"}));

    std::string error;
    // Model the normal policy result with only graphics.gpu effective here:
    // this isolates the broker endpoint from the separately-tested DRM grant.
    const auto gpuGrantedPlan = makeThirdPartySandboxLaunchPlan(
        application, {"graphics.gpu"}, 45, error);
    ASSERT_TRUE(gpuGrantedPlan.has_value()) << error;
    const auto gpuRemovedPlan = makeThirdPartySandboxLaunchPlan(application, {}, 46, error);
    ASSERT_TRUE(gpuRemovedPlan.has_value()) << error;

    SandboxLaunchMaterialInput input{
        .application = application,
        .filesystemSources = seed.filesystemSources,
        .executableBundlePath = seed.executableBundlePath,
        .executableDescriptor = seed.executableDescriptor,
        .arguments = seed.arguments,
    };
    input.filesystemSources.gpuSocketDescriptor = gpuSocketDescriptor_;
    SandboxLaunchMaterialRegistry registry;
    ASSERT_TRUE(registry.registerMaterial(input, error)) << error;

    SandboxPlatformHardening hardening{};
    hardening.requireCgroupResourceAccounting = false;
    SandboxChildLaunchSpec grantedSpec{};
    ASSERT_TRUE(registry.makeChildLaunchSpec(*gpuGrantedPlan, hardening, std::nullopt,
                                             grantedSpec, error)) << error;
    EXPECT_GE(grantedSpec.filesystemSources.gpuSocketDescriptor, 0);

    SandboxChildLaunchSpec removedSpec{};
    ASSERT_TRUE(registry.makeChildLaunchSpec(*gpuRemovedPlan, hardening, std::nullopt,
                                             removedSpec, error)) << error;
    EXPECT_EQ(removedSpec.filesystemSources.gpuSocketDescriptor, -1);
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

TEST_F(SandboxChildLauncherTest, RejectsRetainedMaterialAfterGraphicsEndpointRestart) {
    ASSERT_GE(executableDescriptor_, 0);
    SandboxChildLaunchSpec protectedSpec = validSpec();
    SandboxLaunchMaterialRegistry registry;
    const VerifiedApplication application{
        .appId = protectedSpec.identity.appId,
        .identity = protectedSpec.identity,
        .runtime = protectedSpec.plan.profile.runtime,
        .requestedPermissions = {},
        .bundleRecordDigest = protectedSpec.plan.request.bundleRecordDigest,
        .permissionSubject = std::nullopt,
    };
    const SandboxLaunchMaterialInput input{
        .application = application,
        .filesystemSources = protectedSpec.filesystemSources,
        .executableBundlePath = protectedSpec.executableBundlePath,
        .executableDescriptor = protectedSpec.executableDescriptor,
        .arguments = protectedSpec.arguments,
    };
    std::string error;
    ASSERT_TRUE(registry.registerMaterial(input, error)) << error;

    const fs::path path = executablePath_.parent_path() / "compositor.sock";
    ASSERT_EQ(unlink(path.c_str()), 0);
    replacementCompositorSocketFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(replacementCompositorSocketFd_, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(replacementCompositorSocketFd_, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)), 0);
    ASSERT_EQ(chown(path.c_str(), kSessionUserUid, kApplicationRuntimeGid), 0);
    ASSERT_EQ(chmod(path.c_str(), 0660), 0);

    SandboxChildLaunchSpec rebuilt{};
    SandboxPlatformHardening hardening{};
    hardening.requireCgroupResourceAccounting = false;
    EXPECT_FALSE(registry.runtimeEndpointsCurrent(error));
    EXPECT_NE(error.find("generation changed"), std::string::npos);
    EXPECT_FALSE(registry.makeChildLaunchSpec(protectedSpec.plan, hardening, std::nullopt,
                                              rebuilt, error));
    EXPECT_NE(error.find("generation changed"), std::string::npos);
    registry.removeStaleRuntimeEndpointMaterials();
    EXPECT_TRUE(registry.runtimeEndpointsCurrent(error)) << error;
    EXPECT_FALSE(registry.hasMaterialFor(protectedSpec.plan));
}

} // namespace
} // namespace lcl::security
