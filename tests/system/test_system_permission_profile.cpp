#include <gtest/gtest.h>

#include "system/security/system_permission_profile.hpp"

namespace lcl::security {
namespace {

TEST(SystemPermissionProfileTest, GrantsOnlyQtSmokesExplicitGpuRequest) {
    const std::vector<std::string> grants = staticSystemImagePermissionGrants(
        "org.lcl.qt.smoke", {"graphics.gpu", "graphics.render-node"});

    EXPECT_EQ(grants, std::vector<std::string>({"graphics.gpu", "graphics.render-node"}));

    // The profile never invents a permission the bundle did not request.
    EXPECT_EQ(staticSystemImagePermissionGrants("org.lcl.qt.smoke", {"graphics.render-node"}),
              std::vector<std::string>({"graphics.render-node"}));
    EXPECT_TRUE(staticSystemImagePermissionGrants("org.lcl.sandbox-test", {"graphics.gpu"}).empty());
}

TEST(SystemPermissionProfileTest, GrantsGpuOnlyToExplicitPresentationPocRequest) {
    EXPECT_EQ(staticSystemImagePermissionGrants(
                  "org.lcl.gpu-presentation-probe", {"graphics.gpu"}),
              std::vector<std::string>({"graphics.gpu"}));
    EXPECT_TRUE(staticSystemImagePermissionGrants(
                    "org.lcl.gpu-presentation-probe", {"graphics.render-node"})
                    .empty());
}

} // namespace
} // namespace lcl::security
