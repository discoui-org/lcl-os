#include <gtest/gtest.h>

#include "system/security/sandbox_platform_probe.hpp"

namespace lcl::security {

TEST(SandboxPlatformProbeTest, FormatsEveryMandatoryBoundary) {
    SandboxPlatformCapabilities capabilities{};
    capabilities.mountNamespace = true;
    capabilities.pidNamespace = true;
    capabilities.ipcNamespace = true;
    capabilities.networkNamespace = true;
    capabilities.namespaceCombination = true;
    capabilities.noNewPrivileges = true;
    capabilities.seccompFilter = true;
    capabilities.landlockAbi = 6;
    capabilities.cgroupV2 = true;
    capabilities.cgroupMemoryController = true;
    capabilities.cgroupPidsController = true;
    capabilities.cgroupCpuController = true;
    EXPECT_TRUE(capabilities.supportsCommonCapabilityProfile());
    EXPECT_TRUE(capabilities.supportsMandatoryThirdPartyProfile());

    const std::string report = formatSandboxPlatformCapabilities(capabilities);
    EXPECT_NE(report.find("mount namespace"), std::string::npos);
    EXPECT_NE(report.find("Landlock ABI: 6"), std::string::npos);
    EXPECT_NE(report.find("cgroup memory controller: available"), std::string::npos);
    EXPECT_NE(report.find("common capability profile: supported"), std::string::npos);
    EXPECT_NE(report.find("Linux full hardening profile: supported"), std::string::npos);
}

TEST(SandboxPlatformProbeTest, FailsClosedWhenAnyMandatoryBoundaryIsAbsent) {
    SandboxPlatformCapabilities capabilities{};
    capabilities.mountNamespace = true;
    capabilities.pidNamespace = true;
    capabilities.ipcNamespace = true;
    capabilities.networkNamespace = true;
    capabilities.namespaceCombination = true;
    capabilities.noNewPrivileges = true;
    capabilities.seccompFilter = true;
    capabilities.cgroupV2 = true;
    EXPECT_TRUE(capabilities.supportsCommonCapabilityProfile());
    EXPECT_FALSE(capabilities.supportsMandatoryThirdPartyProfile());

    std::string error;
    const auto androidHardening = makeSandboxPlatformHardening(
        capabilities, SandboxPlatformMode::AndroidCapability, error);
    ASSERT_TRUE(androidHardening.has_value()) << error;
    EXPECT_FALSE(androidHardening->privatePidNamespace);
    EXPECT_FALSE(androidHardening->requireLandlock);
    EXPECT_FALSE(androidHardening->requireCgroupResourceAccounting);

    const auto linuxHardening = makeSandboxPlatformHardening(
        capabilities, SandboxPlatformMode::LinuxFull, error);
    EXPECT_FALSE(linuxHardening.has_value());
}

TEST(SandboxPlatformProbeTest, SelectsLocalHardeningBelowTheCommonCapabilityABI) {
    SandboxPlatformCapabilities capabilities{};
    capabilities.mountNamespace = true;
    capabilities.pidNamespace = true;
    capabilities.ipcNamespace = true;
    capabilities.networkNamespace = true;
    capabilities.namespaceCombination = true;
    capabilities.noNewPrivileges = true;
    capabilities.seccompFilter = true;
    capabilities.landlockAbi = 4;
    capabilities.cgroupV2 = true;
    capabilities.cgroupMemoryController = true;
    capabilities.cgroupPidsController = true;
    capabilities.cgroupCpuController = true;

    std::string error;
    const auto linuxHardening = makeSandboxPlatformHardening(
        capabilities, SandboxPlatformMode::LinuxFull, error);
    ASSERT_TRUE(linuxHardening.has_value()) << error;
    EXPECT_TRUE(linuxHardening->privateMountNamespace);
    EXPECT_TRUE(linuxHardening->privateNetworkNamespace);
    EXPECT_TRUE(linuxHardening->requirePortableResourceLimits);
    EXPECT_TRUE(linuxHardening->privatePidNamespace);
    EXPECT_TRUE(linuxHardening->privateIpcNamespace);
    EXPECT_TRUE(linuxHardening->requireLandlock);
    EXPECT_TRUE(linuxHardening->requireCgroupResourceAccounting);

    const auto androidHardening = makeSandboxPlatformHardening(
        capabilities, SandboxPlatformMode::AndroidCapability, error);
    ASSERT_TRUE(androidHardening.has_value()) << error;
    EXPECT_FALSE(androidHardening->privatePidNamespace);
    EXPECT_FALSE(androidHardening->privateIpcNamespace);
    EXPECT_FALSE(androidHardening->requireLandlock);
    EXPECT_FALSE(androidHardening->requireCgroupResourceAccounting);
}

} // namespace lcl::security
