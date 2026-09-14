#include <gtest/gtest.h>

#include "system/shells/permission_prompt.hpp"

namespace lcl::shell {

TEST(PermissionPromptLayoutTest, CentersDesktopSheet) {
    const auto layout = layoutPermissionPrompt(
        PermissionPromptPresentation::Desktop, 1280.0f, 800.0f);

    EXPECT_FLOAT_EQ(layout.width, 440.0f);
    EXPECT_FLOAT_EQ(layout.height, 244.0f);
    EXPECT_FLOAT_EQ(layout.x, 420.0f);
    EXPECT_FLOAT_EQ(layout.y, 278.0f);
    EXPECT_FALSE(layout.topCornersOnly);
}

TEST(PermissionPromptLayoutTest, PinsMobileSheetToBottom) {
    const auto layout = layoutPermissionPrompt(
        PermissionPromptPresentation::Mobile, 412.0f, 915.0f);

    EXPECT_FLOAT_EQ(layout.x, 0.0f);
    EXPECT_FLOAT_EQ(layout.y + layout.height, 915.0f);
    EXPECT_FLOAT_EQ(layout.width, 412.0f);
    EXPECT_FLOAT_EQ(layout.height, 278.0f);
    EXPECT_TRUE(layout.topCornersOnly);
}

TEST(PermissionPromptLayoutTest, StaysInsideSmallOutputs) {
    const auto desktop = layoutPermissionPrompt(
        PermissionPromptPresentation::Desktop, 20.0f, 20.0f);
    const auto mobile = layoutPermissionPrompt(
        PermissionPromptPresentation::Mobile, 20.0f, 20.0f);

    EXPECT_GE(desktop.x, 0.0f);
    EXPECT_GE(desktop.y, 0.0f);
    EXPECT_LE(desktop.x + desktop.width, 20.0f);
    EXPECT_LE(desktop.y + desktop.height, 20.0f);
    EXPECT_FLOAT_EQ(mobile.y, 0.0f);
    EXPECT_FLOAT_EQ(mobile.height, 20.0f);
}

} // namespace lcl::shell
