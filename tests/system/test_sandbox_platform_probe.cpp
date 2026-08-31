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
    EXPECT_TRUE(capabilities.supportsMandatoryThirdPartyProfile());

    const std::string report = formatSandboxPlatformCapabilities(capabilities);
    EXPECT_NE(report.find("mount namespace"), std::string::npos);
    EXPECT_NE(report.find("Landlock ABI: 6"), std::string::npos);
    EXPECT_NE(report.find("third-party profile: supported"), std::string::npos);
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
    EXPECT_FALSE(capabilities.supportsMandatoryThirdPartyProfile());
}

} // namespace lcl::security
