#include <gtest/gtest.h>

#include "core/compositor/windowing_policy.hpp"

namespace lcl::core {
namespace {

TEST(WindowingPolicyTest, DesktopPreservesRequestedWindowContract) {
    DesktopWindowPolicy policy;
    float x = 80.0f;
    float y = 60.0f;
    float width = 540.0f;
    float height = 360.0f;
    auto decorationMode = protocol::LCLDecorationMode::CSD;
    bool insetBorderEnabled = true;

    policy.configureNormalSurface(
        1200.0f, 800.0f, x, y, width, height,
        decorationMode, insetBorderEnabled);

    EXPECT_FLOAT_EQ(x, 80.0f);
    EXPECT_FLOAT_EQ(y, 60.0f);
    EXPECT_FLOAT_EQ(width, 540.0f);
    EXPECT_FLOAT_EQ(height, 360.0f);
    EXPECT_EQ(decorationMode, protocol::LCLDecorationMode::CSD);
    EXPECT_TRUE(insetBorderEnabled);
    EXPECT_EQ(policy.resolveDecorationMode(protocol::LCLDecorationMode::SSD),
              protocol::LCLDecorationMode::SSD);
    EXPECT_TRUE(policy.resolveInsetBorderEnabled(true));
    EXPECT_FLOAT_EQ(policy.resolveWindowCornerRadius(14.0f), 14.0f);
    EXPECT_TRUE(policy.usesDesktopWindowManagement());
    EXPECT_FALSE(policy.usesSystemGestures());
    EXPECT_FALSE(policy.forcesOpaqueNormalSurfaces());
}

TEST(WindowingPolicyTest, MobileForcesFullscreenFramelessWindowContract) {
    MobileWindowPolicy policy;
    float x = 80.0f;
    float y = 60.0f;
    float width = 540.0f;
    float height = 360.0f;
    auto decorationMode = protocol::LCLDecorationMode::SSD;
    bool insetBorderEnabled = true;

    policy.configureNormalSurface(
        1200.0f, 800.0f, x, y, width, height,
        decorationMode, insetBorderEnabled);

    EXPECT_FLOAT_EQ(x, 0.0f);
    EXPECT_FLOAT_EQ(y, 0.0f);
    EXPECT_FLOAT_EQ(width, 1200.0f);
    EXPECT_FLOAT_EQ(height, 800.0f);
    EXPECT_EQ(decorationMode, protocol::LCLDecorationMode::None);
    EXPECT_FALSE(insetBorderEnabled);
    EXPECT_EQ(policy.resolveDecorationMode(protocol::LCLDecorationMode::SSD),
              protocol::LCLDecorationMode::None);
    EXPECT_EQ(policy.resolveDecorationMode(protocol::LCLDecorationMode::CSD),
              protocol::LCLDecorationMode::None);
    EXPECT_FALSE(policy.resolveInsetBorderEnabled(true));
    EXPECT_FLOAT_EQ(policy.resolveWindowCornerRadius(20.0f), 0.0f);
    EXPECT_FALSE(policy.usesDesktopWindowManagement());
    EXPECT_TRUE(policy.usesSystemGestures());
    EXPECT_TRUE(policy.forcesOpaqueNormalSurfaces());
}

TEST(WindowingPolicyTest, GestaltShellKindSelectsMatchingPolicy) {
    const auto desktop = makeWindowingPolicy(lcl::platform::ShellKind::Desktop);
    const auto mobile = makeWindowingPolicy(lcl::platform::ShellKind::Mobile);

    ASSERT_NE(desktop, nullptr);
    ASSERT_NE(mobile, nullptr);
    EXPECT_EQ(desktop->shellKind(), lcl::platform::ShellKind::Desktop);
    EXPECT_EQ(mobile->shellKind(), lcl::platform::ShellKind::Mobile);
}

} // namespace
} // namespace lcl::core
