#include <gtest/gtest.h>

#include "system/security/sandbox_child_launcher.hpp"

#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lcl::security {
namespace {

class SandboxChildLauncherTest : public ::testing::Test {
protected:
    fs::path executablePath_;
    int executableDescriptor_{-1};

    void SetUp() override {
        const fs::path directory = fs::temp_directory_path() /
                                   ("lcl_sandbox_child_" + std::to_string(getpid()));
        fs::create_directories(directory);
        executablePath_ = directory / "app";
        std::ofstream executable(executablePath_);
        executable << "#!/bin/sh\nexit 0\n";
        executable.close();
        chmod(executablePath_.c_str(), 0700);
        executableDescriptor_ = open(executablePath_.c_str(), O_RDONLY | O_CLOEXEC);
    }

    void TearDown() override {
        if (executableDescriptor_ >= 0) {
            close(executableDescriptor_);
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
        spec.identity = application.identity;
        spec.kernelEnforcement = {
            .mountNamespaceReady = true,
            .pidNamespaceReady = true,
            .ipcNamespaceReady = true,
            .networkNamespaceReady = true,
            .seccompInstalled = true,
            .landlockInstalled = true,
            .directDeviceAccessDenied = true,
        };
        spec.executableDescriptor = executableDescriptor_;
        spec.arguments = {"--safe"};
        return spec;
    }
};

TEST_F(SandboxChildLauncherTest, AcceptsOnlyProtectedFullyEnforcedLaunchMaterial) {
    ASSERT_GE(executableDescriptor_, 0);
    SandboxChildLaunchSpec spec = validSpec();
    std::string error;
    EXPECT_TRUE(validateSandboxChildLaunchSpec(spec, error)) << error;

    spec.kernelEnforcement.seccompInstalled = false;
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

} // namespace
} // namespace lcl::security
