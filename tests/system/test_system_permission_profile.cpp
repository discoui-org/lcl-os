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

} // namespace
} // namespace lcl::security
