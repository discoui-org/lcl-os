#include <gtest/gtest.h>

#include <algorithm>

#include "render/window_manager.hpp"

namespace {

const lcl::render::Window* findWindow(const lcl::render::WindowManager& manager,
                                      uint32_t windowId) {
    const auto& windows = manager.getWindows();
    const auto it = std::find_if(windows.begin(), windows.end(), [windowId](const lcl::render::Window& window) {
        return window.id == windowId;
    });
    return it == windows.end() ? nullptr : &*it;
}

} // namespace

TEST(WindowManagerTest, WindowActionsPreserveRestoreGeometryAndFocus) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);

    const uint32_t first = manager.createWindow("First", 80, 80, 400, 300);
    const uint32_t second = manager.createWindow("Second", 160, 120, 320, 240);
    ASSERT_EQ(manager.getFocusedWindowId(), second);

    ASSERT_TRUE(manager.maximizeWindow(first));
    const auto* maximized = findWindow(manager, first);
    ASSERT_NE(maximized, nullptr);
    EXPECT_TRUE(maximized->isMaximized);
    EXPECT_FALSE(maximized->isMinimized);
    EXPECT_EQ(maximized->x, 0);
    EXPECT_EQ(maximized->y, 32);
    EXPECT_EQ(maximized->width, 1000);
    EXPECT_EQ(maximized->height, 608);
    EXPECT_EQ(manager.getFocusedWindowId(), first);

    ASSERT_TRUE(manager.minimizeWindow(first));
    const auto* minimized = findWindow(manager, first);
    ASSERT_NE(minimized, nullptr);
    EXPECT_TRUE(minimized->isMinimized);
    EXPECT_TRUE(minimized->isMaximized);
    EXPECT_EQ(manager.getFocusedWindowId(), second);

    ASSERT_TRUE(manager.restoreWindow(first));
    const auto* restoredMaximized = findWindow(manager, first);
    ASSERT_NE(restoredMaximized, nullptr);
    EXPECT_FALSE(restoredMaximized->isMinimized);
    EXPECT_TRUE(restoredMaximized->isMaximized);
    EXPECT_EQ(manager.getFocusedWindowId(), first);

    ASSERT_TRUE(manager.toggleMaximizeWindow(first));
    const auto* restored = findWindow(manager, first);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->isMaximized);
    EXPECT_EQ(restored->x, 80);
    EXPECT_EQ(restored->y, 80);
    EXPECT_EQ(restored->width, 400);
    EXPECT_EQ(restored->height, 300);

    ASSERT_TRUE(manager.beginWindowDrag(first, 24, 18));
    const auto* dragging = findWindow(manager, first);
    ASSERT_NE(dragging, nullptr);
    EXPECT_TRUE(dragging->isDragging);
    EXPECT_EQ(dragging->dragOffsetX, 24);
    EXPECT_EQ(dragging->dragOffsetY, 18);
}

TEST(WindowManagerTest, UnfocusableSystemWindowDoesNotStealApplicationFocus) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));

    const uint32_t appWindow = manager.createWindow("App", 80, 80, 400, 300);
    ASSERT_EQ(manager.getFocusedWindowId(), appWindow);

    const uint32_t panel = manager.createWindow(
        "Panel", 0, 0, 1000, 32, 0xFF38BDF8, false);
    manager.setWindowLayer(panel, lcl::protocol::LCLWindowLayer::TopMost, true);

    EXPECT_EQ(manager.getFocusedWindowId(), appWindow);
    const auto* panelWindow = findWindow(manager, panel);
    ASSERT_NE(panelWindow, nullptr);
    EXPECT_TRUE(panelWindow->isUnfocusable);
    EXPECT_FALSE(panelWindow->isFocused);
}

TEST(WindowManagerTest, WindowCornerStyleOwnsRadiusAndRoundnessTogether) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Styled", 80, 80, 400, 300);

    manager.setWindowCornerStyle(id, 20.0f, 3.2f);
    const auto* window = findWindow(manager, id);
    ASSERT_NE(window, nullptr);
    EXPECT_FLOAT_EQ(window->cornerRadiusPx, 20.0f);
    EXPECT_FLOAT_EQ(window->cornerRoundness, 3.2f);
}

TEST(WindowManagerTest, MaximizeRestoreMorphRetargetsFromPresentationGeometry) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);
    const uint32_t id = manager.createWindow("Morph", 80, 90, 400, 300);

    ASSERT_TRUE(manager.maximizeWindow(id));
    const auto* logical = findWindow(manager, id);
    ASSERT_NE(logical, nullptr);
    EXPECT_EQ(logical->x, 0);
    EXPECT_FLOAT_EQ(logical->presentationX, 80.0f);
    manager.updateAnimations(0.10f);
    const float midWidth = findWindow(manager, id)->presentationWidth;
    EXPECT_GT(midWidth, 400.0f);
    EXPECT_LT(midWidth, 1000.0f);

    ASSERT_TRUE(manager.restoreWindow(id));
    EXPECT_NEAR(findWindow(manager, id)->presentationWidth, midWidth, 0.001f);
    for (int index = 0; index < 240; ++index) manager.updateAnimations(1.0f / 240.0f);
    const auto* restored = findWindow(manager, id);
    EXPECT_FALSE(restored->geometryTransitionActive);
    EXPECT_NEAR(restored->presentationX, 80.0f, 0.01f);
    EXPECT_NEAR(restored->presentationY, 90.0f, 0.01f);
    EXPECT_NEAR(restored->presentationWidth, 400.0f, 0.01f);
    EXPECT_NEAR(restored->presentationHeight, 300.0f, 0.01f);
}

TEST(WindowManagerTest, IntermediateResizeCommitPreservesNewerPointerTarget) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Resize", 80, 90, 400, 300);
    auto& window = manager.getWindowsMutable().back();
    window.pendingWidth = 560;
    window.pendingHeight = 520;
    window.activeResizeEdge = lcl::render::ResizeEdge::Bottom;
    window.anchorBottom = window.y + window.pendingHeight;

    manager.commitSurfaceGeometry(id, 560, 420, true);
    const auto* intermediate = findWindow(manager, id);
    ASSERT_NE(intermediate, nullptr);
    EXPECT_EQ(intermediate->height, 420);
    EXPECT_EQ(intermediate->pendingHeight, 520);
    EXPECT_EQ(intermediate->activeResizeEdge, lcl::render::ResizeEdge::Bottom);

    manager.commitSurfaceGeometry(id, 560, 520, false);
    const auto* final = findWindow(manager, id);
    ASSERT_NE(final, nullptr);
    EXPECT_EQ(final->pendingHeight, 520);
    EXPECT_EQ(final->activeResizeEdge, lcl::render::ResizeEdge::None);
}

TEST(WindowManagerTest, ServerChromeControlsAnimateHoverPressWithoutGlyphState) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t id = manager.createWindow("Chrome", 80, 60, 400, 300);

    lcl::core::InputEvent move{};
    move.type = lcl::core::InputEventType::PointerMotion;
    move.absoluteX = 100.0;
    move.absoluteY = 80.0;
    EXPECT_TRUE(manager.processInputEvent(move));
    ASSERT_EQ(findWindow(manager, id)->chrome.hoveredControl(), 0);
    for (int index = 0; index < 60; ++index) manager.updateAnimations(1.0f / 240.0f);
    EXPECT_GT(findWindow(manager, id)->chrome.control(0).scale, 1.0f);

    lcl::core::InputEvent down{};
    down.type = lcl::core::InputEventType::PointerButton;
    down.pressed = true;
    EXPECT_TRUE(manager.processInputEvent(down));
    ASSERT_EQ(findWindow(manager, id)->chrome.pressedControl(), 0);
    EXPECT_FALSE(findWindow(manager, id)->closeRequested);
    for (int index = 0; index < 60; ++index) manager.updateAnimations(1.0f / 240.0f);
    EXPECT_LT(findWindow(manager, id)->chrome.control(0).scale, 1.0f);

    lcl::core::InputEvent up{};
    up.type = lcl::core::InputEventType::PointerButton;
    up.pressed = false;
    EXPECT_TRUE(manager.processInputEvent(up));
    ASSERT_EQ(findWindow(manager, id)->chrome.pressedControl(), -1);
    for (int index = 0; index < 90; ++index) manager.updateAnimations(1.0f / 240.0f);
    EXPECT_GT(findWindow(manager, id)->chrome.control(0).scale, 1.0f);
}
