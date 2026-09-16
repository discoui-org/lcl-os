#include <gtest/gtest.h>

#include "system/security/system_permission_profile.hpp"

namespace lcl::security {
namespace {

TEST(SystemPermissionProfileTest, DeniesGpuToRemovedIntegrationBundles) {
    for (const auto* appId : {"org.lcl.qt.smoke", "org.lcl.client-gpu-probe",
                              "org.lcl.gpu-presentation-probe", "org.lcl.sandbox-test"}) {
        EXPECT_TRUE(staticSystemImagePermissionGrants(
                        appId, {"graphics.gpu", "graphics.render-node"})
                        .empty());
    }
}

TEST(SystemPermissionProfileTest, GrantsOnlyVulkanGearsExplicitGpuRequests) {
    EXPECT_EQ(staticSystemImagePermissionGrants(
                  "org.lcl.vulkan-gears", {"graphics.gpu", "graphics.render-node"}),
              (std::vector<std::string>{"graphics.gpu", "graphics.render-node"}));
    EXPECT_EQ(staticSystemImagePermissionGrants(
                  "org.lcl.vulkan-gears", {"graphics.render-node"}),
              std::vector<std::string>({"graphics.render-node"}));
    EXPECT_TRUE(staticSystemImagePermissionGrants(
                    "org.lcl.gfxstream-vulkan-test", {"graphics.gpu"})
                    .empty());
}

} // namespace
} // namespace lcl::security
