#include <gtest/gtest.h>
#include "core/display/display_scale.hpp"
#include <cstdlib>

using namespace lcl::core;

TEST(DisplayScaleTest, DefaultScaleOne) {
    unsetenv("LCL_SCALE");
    DisplayScale::initialize();
    
    EXPECT_FLOAT_EQ(DisplayScale::factor(), 1.0f);
    EXPECT_EQ(DisplayScale::px(100), 100);
    EXPECT_EQ(DisplayScale::menuBarHeight(), DisplayScale::kMenuBarHeight);
    EXPECT_EQ(DisplayScale::titleBarHeight(), DisplayScale::kTitleBarHeight);
}

TEST(DisplayScaleTest, EnvironmentScaleOverride) {
    setenv("LCL_SCALE", "2.0", 1);
    DisplayScale::initialize();

    EXPECT_FLOAT_EQ(DisplayScale::factor(), 2.0f);
    EXPECT_EQ(DisplayScale::px(100), 200);
    EXPECT_EQ(DisplayScale::menuBarHeight(), 80);
    EXPECT_EQ(DisplayScale::titleBarHeight(), 64);

    unsetenv("LCL_SCALE");
}
